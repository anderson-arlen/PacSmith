package httpapi

import (
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"strconv"

	"github.com/anderson-arlen/pacsmith/server/internal/acp"
	"github.com/anderson-arlen/pacsmith/server/internal/auth"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func (s *Server) acpError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, acp.ErrConflict):
		writeError(w, 409, "conflict", err.Error())
	case errors.Is(err, acp.ErrInvalid):
		writeError(w, 400, "bad_request", err.Error())
	case errors.Is(err, sql.ErrNoRows):
		writeError(w, 404, "not_found", "conversation or request not found")
	default:
		writeRequestError(w, err)
	}
}
func conversationJSON(row sqlcdb.AcpConversation) map[string]any {
	return map[string]any{"id": row.ID, "project_id": row.ProjectID, "release_id": row.ReleaseID, "automatic": row.Automatic != 0, "title": row.Title, "status": row.Status, "error": row.Error, "session_id": row.SessionID, "config_options": json.RawMessage(row.ConfigJson), "created_at": row.CreatedAt, "updated_at": row.UpdatedAt}
}
func permissionJSON(row sqlcdb.AcpPermission) map[string]any {
	var request map[string]any
	_ = json.Unmarshal([]byte(row.RequestJson), &request)
	return map[string]any{"id": row.ID, "conversation_id": row.ConversationID, "params": request["params"], "created_at": row.CreatedAt}
}
func (s *Server) acpAPI(w http.ResponseWriter, r *http.Request) {
	if s.ACP == nil {
		writeError(w, 503, "unavailable", "server ACP is unavailable")
		return
	}
	ctx := r.Context()
	id := r.PathValue("id")
	action := r.PathValue("action")
	switch r.Pattern {
	case "GET /api/v1/ai/settings":
		cfg, err := s.ACP.Settings(ctx)
		if err != nil {
			s.acpError(w, err)
			return
		}
		writeJSON(w, 200, cfg)
	case "PUT /api/v1/ai/settings":
		var cfg acp.Settings
		if !decodeJSON(w, r, &cfg) {
			return
		}
		updated, err := s.ACP.Configure(ctx, cfg)
		if err != nil {
			s.acpError(w, err)
			return
		}
		writeJSON(w, 200, updated)
	case "GET /api/v1/ai/conversations":
		rows, err := s.ACP.List(ctx, r.URL.Query().Get("project_id"))
		if err != nil {
			s.acpError(w, err)
			return
		}
		result := []any{}
		for _, row := range rows {
			result = append(result, conversationJSON(row))
		}
		writeJSON(w, 200, map[string]any{"conversations": result})
	case "POST /api/v1/ai/conversations":
		var req acp.CreateRequest
		if !decodeJSON(w, r, &req) {
			return
		}
		row, err := s.ACP.Create(ctx, req)
		if err != nil {
			s.acpError(w, err)
			return
		}
		writeJSON(w, 200, conversationJSON(row))
	case "GET /api/v1/ai/conversations/{id}":
		after, _ := strconv.ParseInt(r.URL.Query().Get("after"), 10, 64)
		snapshot, err := s.ACP.Snapshot(ctx, id, after)
		if err != nil {
			s.acpError(w, err)
			return
		}
		result := conversationJSON(snapshot.Conversation)
		ev := []any{}
		for _, e := range snapshot.Events {
			ev = append(ev, map[string]any{"id": e.ID, "kind": e.Kind, "body": json.RawMessage(e.BodyJson)})
		}
		result["events"] = ev
		pending := []any{}
		for _, p := range snapshot.Permissions {
			if p.ConversationID == id {
				pending = append(pending, permissionJSON(p))
			}
		}
		result["permissions"] = pending
		writeJSON(w, 200, result)
	case "GET /api/v1/ai/permissions":
		rows, err := s.ACP.Permissions(ctx)
		if err != nil {
			s.acpError(w, err)
			return
		}
		result := []any{}
		for _, row := range rows {
			result = append(result, permissionJSON(row))
		}
		writeJSON(w, 200, map[string]any{"permissions": result})
	case "POST /api/v1/ai/permissions/{id}/response":
		var req struct {
			Option string `json:"option_id"`
		}
		if !decodeJSON(w, r, &req) {
			return
		}
		p := auth.PrincipalFrom(ctx)
		principal := p.ClientID
		if p.IsLocalAdmin() {
			principal = "local"
		}
		if err := s.ACP.Answer(ctx, id, req.Option, principal); err != nil {
			s.acpError(w, err)
			return
		}
		writeJSON(w, 200, map[string]bool{"resolved": true})
	case "POST /api/v1/ai/conversations/{id}/{action}":
		var req struct {
			Display  *string          `json:"display_text"`
			Content  []map[string]any `json:"content"`
			ConfigID string           `json:"config_id"`
			Value    any              `json:"value"`
			Method   string           `json:"method"`
			Title    string           `json:"title"`
		}
		if !decodeJSONLimit(w, r, &req, 60<<20) {
			return
		}
		var err error
		switch action {
		case "initialize":
			err = s.ACP.Initialize(id)
		case "prompt":
			err = s.ACP.PromptWithDisplay(id, req.Content, req.Display)
		case "cancel":
			err = s.ACP.Cancel(id)
		case "configure":
			err = s.ACP.ConfigureSession(id, req.ConfigID, req.Value)
		case "authenticate":
			err = s.ACP.Authenticate(id, req.Method)
		case "clear-permissions":
			err = s.ACP.ClearGrants(ctx, id)
		case "title":
			err = s.ACP.Title(ctx, id, req.Title)
		default:
			writeError(w, 404, "not_found", "unknown conversation action")
			return
		}
		if err != nil {
			s.acpError(w, err)
			return
		}
		writeJSON(w, 202, map[string]bool{"accepted": true})
	}
}
