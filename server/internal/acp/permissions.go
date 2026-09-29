package acp

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"regexp"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

func identity(profile string) string {
	sum := sha256.Sum256([]byte(profile))
	return hex.EncodeToString(sum[:])
}
func toolName(params map[string]any) string {
	call := object(params["toolCall"])
	input := object(call["rawInput"])
	name := str(input["tool"])
	if object(call["_meta"])["is_mcp_tool_call"] != true || input["server"] != "pacsmith_session" || !regexp.MustCompile(`^[a-z][a-z0-9_]*$`).MatchString(name) {
		return ""
	}
	return name
}
func (r *runner) permission(id any, params map[string]any) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	cancel := func() error {
		return r.send(map[string]any{"jsonrpc": "2.0", "id": id, "result": map[string]any{"outcome": map[string]any{"outcome": "cancelled"}}})
	}
	if r.cancelled || r.replaying || (r.row.Status != "running" && r.row.Status != "waiting") || params["sessionId"] != r.row.SessionID || r.row.SessionID == "" {
		return cancel()
	}
	call := object(params["toolCall"])
	merged := map[string]any{}
	for k, v := range r.tools[str(call["toolCallId"])] {
		merged[k] = v
	}
	for k, v := range call {
		merged[k] = v
	}
	params["toolCall"] = merged
	options, _ := params["options"].([]any)
	seen := map[string]bool{}
	if len(options) == 0 {
		return cancel()
	}
	for _, v := range options {
		o := object(v)
		key := str(o["optionId"])
		kind := str(o["kind"])
		if key == "" || seen[key] || (kind != "allow_once" && kind != "allow_always" && kind != "reject_once" && kind != "reject_always") {
			return cancel()
		}
		seen[key] = true
	}
	tool := toolName(params)
	if r.codex && tool != "" {
		count, err := r.service.DB.Queries.HasACPGrant(r.service.ctx, sqlcdb.HasACPGrantParams{Identity: identity(r.row.ProfileJson), Tool: tool, SessionID: r.row.ID})
		if err != nil {
			return err
		}
		if count > 0 {
			for _, v := range options {
				o := object(v)
				if o["kind"] == "allow_once" {
					_ = r.service.append(r.service.ctx, r.row.ID, "notice", map[string]any{"message": "Allowed by saved permission: " + str(merged["title"])})
					return r.send(map[string]any{"jsonrpc": "2.0", "id": id, "result": map[string]any{"outcome": map[string]any{"outcome": "selected", "optionId": o["optionId"]}}})
				}
			}
		}
		for _, v := range options {
			o := object(v)
			if o["kind"] == "allow_always" {
				if o["optionId"] == "allow_session" {
					o["pacsmithDescription"] = "Remember this tool for this server conversation."
				}
				if o["optionId"] == "allow_always" {
					o["pacsmithDescription"] = "Remember this tool for future conversations with this server agent."
				}
			}
		}
	}
	_, err := r.service.DB.Queries.CreateACPPermission(r.service.ctx, sqlcdb.CreateACPPermissionParams{ID: uuid.NewString(), ConversationID: r.row.ID, RequestJson: encode(map[string]any{"id": id, "params": params}), CreatedAt: now()})
	if err != nil {
		return err
	}
	r.row.Status = "waiting"
	if err := r.service.save(r.service.ctx, r.row); err != nil {
		return err
	}
	r.service.publish()
	return nil
}
func (s *Service) Answer(ctx context.Context, id, option, principal string) error {
	permission, err := s.DB.Queries.GetACPPermission(ctx, id)
	if err != nil {
		return err
	}
	s.mu.Lock()
	r := s.runners[permission.ConversationID]
	s.mu.Unlock()
	if r == nil {
		return ErrConflict
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	var request map[string]any
	if err := json.Unmarshal([]byte(permission.RequestJson), &request); err != nil {
		return err
	}
	params := object(request["params"])
	options, _ := params["options"].([]any)
	kind := ""
	if option != "" {
		for _, v := range options {
			o := object(v)
			if o["optionId"] == option {
				kind = str(o["kind"])
			}
		}
	}
	if option != "" && kind == "" {
		return ErrInvalid
	}
	status := "selected"
	if option == "" {
		status = "cancelled"
	}
	// The compare-and-set is shared by all clients, including reconnects and duplicate clicks.
	_, err = s.DB.Queries.ResolveACPPermission(ctx, sqlcdb.ResolveACPPermissionParams{ID: id, Status: status, OptionID: option, AnsweredBy: principal, ResolvedAt: now()})
	if errors.Is(err, sql.ErrNoRows) {
		return ErrConflict
	}
	if err != nil {
		return err
	}
	outcome := map[string]any{"outcome": status}
	if option != "" {
		outcome["optionId"] = option
	}
	err = r.send(map[string]any{"jsonrpc": "2.0", "id": request["id"], "result": map[string]any{"outcome": outcome}})
	if err == nil && r.codex && kind == "allow_always" && (option == "allow_always" || option == "allow_session") {
		if tool := toolName(params); tool != "" {
			scope := ""
			if option == "allow_session" {
				scope = r.row.ID
			}
			err = s.DB.Queries.RememberACPGrant(ctx, sqlcdb.RememberACPGrantParams{Identity: identity(r.row.ProfileJson), Tool: tool, SessionID: scope})
		}
	}
	r.row.Status = "running"
	_ = s.save(ctx, r.row)
	_ = s.append(ctx, r.row.ID, "permission_resolved", map[string]any{"id": id, "option_id": option, "answered_by": principal})
	s.publish()
	return err
}
