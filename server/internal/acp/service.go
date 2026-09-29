package acp

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/events"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

var ErrConflict = errors.New("ACP request already resolved or conversation busy")
var ErrInvalid = errors.New("invalid ACP request")

type Profile struct {
	Name            string            `json:"name"`
	Executable      string            `json:"executable"`
	Arguments       []string          `json:"arguments"`
	Environment     map[string]string `json:"environment"`
	RegistryID      string            `json:"registryId"`
	RegistryVersion string            `json:"registryVersion"`
	Defaults        map[string]any    `json:"configDefaults"`
	Protocol        string            `json:"protocol"`
}
type Settings struct {
	Revision int64    `json:"revision"`
	Harness  *Profile `json:"harness"`
}
type Service struct {
	DB                     *sqlite.DB
	Library                *library.Service
	Events                 *events.Hub
	Directory, Socket, CLI string
	mu                     sync.Mutex
	runners                map[string]*runner
	ctx                    context.Context
	cancel                 context.CancelFunc
	wg                     sync.WaitGroup
}
type CreateRequest struct {
	ID        string `json:"id"`
	ProjectID string `json:"project_id"`
	ReleaseID string `json:"release_id"`
	Title     string `json:"title"`
}

func now() string         { return time.Now().UTC().Format(time.RFC3339Nano) }
func encode(v any) string { b, _ := json.Marshal(v); return string(b) }
func object(v any) map[string]any {
	m, _ := v.(map[string]any)
	if m == nil {
		return map[string]any{}
	}
	return m
}
func str(v any) string { s, _ := v.(string); return s }
func (s *Service) Start(ctx context.Context) error {
	s.ctx, s.cancel = context.WithCancel(ctx)
	s.runners = map[string]*runner{}
	if err := os.MkdirAll(s.Directory, 0700); err != nil {
		return err
	}
	if err := s.DB.Queries.InvalidateACPPermissions(ctx, now()); err != nil {
		return err
	}
	rows, err := s.DB.Queries.ListActiveACPConversations(ctx)
	if err != nil {
		return err
	}
	for _, row := range rows {
		if err := s.DB.Queries.CancelACPPermissions(ctx, sqlcdb.CancelACPPermissionsParams{ResolvedAt: now(), ConversationID: row.ID}); err != nil {
			return err
		}
		row.Status = "interrupted"
		row.Error = "Server restarted. Send a message to resume this conversation."
		if err := s.save(ctx, row); err != nil {
			return err
		}
	}
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-s.ctx.Done():
				return
			case <-ticker.C:
				s.reviewPending()
				s.reapIdle()
			}
		}
	}()
	return nil
}
func (s *Service) Close() {
	if s.cancel == nil {
		return
	}
	s.cancel()
	s.mu.Lock()
	for _, r := range s.runners {
		r.stop()
	}
	s.mu.Unlock()
	s.wg.Wait()
}
func (s *Service) Settings(ctx context.Context) (Settings, error) {
	row, err := s.DB.Queries.GetACPSettings(ctx)
	if err != nil {
		return Settings{}, err
	}
	out := Settings{Revision: row.Revision}
	var p Profile
	if err := json.Unmarshal([]byte(row.ProfileJson), &p); err != nil {
		return out, err
	}
	if p.Executable != "" {
		out.Harness = &p
	}
	return out, nil
}
func (s *Service) Configure(ctx context.Context, cfg Settings) (Settings, error) {
	raw := "{}"
	if p := cfg.Harness; p != nil {
		p.Name = strings.TrimSpace(p.Name)
		p.Executable = strings.TrimSpace(p.Executable)
		p.Protocol = "acp"
		if p.Name == "" || p.Executable == "" {
			return Settings{}, fmt.Errorf("%w: agent name and server executable are required", ErrInvalid)
		}
		for _, v := range append([]string{p.Name, p.Executable}, p.Arguments...) {
			if strings.ContainsRune(v, 0) || strings.Contains(v, "{prompt}") {
				return Settings{}, ErrInvalid
			}
		}
		for key, value := range p.Defaults {
			if key == "" {
				return Settings{}, ErrInvalid
			}
			switch value.(type) {
			case string, bool:
			default:
				return Settings{}, fmt.Errorf("%w: agent defaults must be strings or booleans", ErrInvalid)
			}
		}
		for k, v := range p.Environment {
			if k == "" || strings.ContainsAny(k, "=\x00") || strings.ContainsRune(v, 0) {
				return Settings{}, ErrInvalid
			}
		}
		raw = encode(p)
	}
	_, err := s.DB.Queries.UpdateACPSettings(ctx, sqlcdb.UpdateACPSettingsParams{ProfileJson: raw, Revision: cfg.Revision})
	if errors.Is(err, sql.ErrNoRows) {
		return Settings{}, ErrConflict
	}
	if err != nil {
		return Settings{}, err
	}
	s.publish()
	return s.Settings(ctx)
}
func (s *Service) publish() { s.Events.Publish(events.Event{Topics: []string{"ai"}}) }
func (s *Service) save(ctx context.Context, row sqlcdb.AcpConversation) error {
	return s.DB.Queries.UpdateACPConversation(ctx, sqlcdb.UpdateACPConversationParams{ID: row.ID, SessionID: row.SessionID, Status: row.Status, ConfigJson: row.ConfigJson, Error: row.Error, UpdatedAt: now()})
}
func (s *Service) append(ctx context.Context, id, kind string, body any) error {
	_, err := s.DB.Queries.AppendACPEvent(ctx, sqlcdb.AppendACPEventParams{ConversationID: id, Kind: kind, BodyJson: encode(body)})
	if err == nil {
		s.publish()
	}
	return err
}
func (s *Service) Create(ctx context.Context, req CreateRequest) (sqlcdb.AcpConversation, error) {
	return s.create(ctx, req, false)
}
func (s *Service) create(ctx context.Context, req CreateRequest, automatic bool) (sqlcdb.AcpConversation, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if req.ID == "" {
		req.ID = uuid.NewString()
	}
	if !regexp.MustCompile(`^[a-zA-Z0-9_-]{1,128}$`).MatchString(req.ID) {
		return sqlcdb.AcpConversation{}, ErrInvalid
	}
	if row, err := s.DB.Queries.GetACPConversation(ctx, req.ID); err == nil {
		return row, nil
	}
	cfg, err := s.Settings(ctx)
	if err != nil {
		return sqlcdb.AcpConversation{}, err
	}
	if cfg.Harness == nil {
		return sqlcdb.AcpConversation{}, fmt.Errorf("%w: configure the server's AI harness first", ErrInvalid)
	}
	auto := int64(0)
	status := "idle"
	if automatic {
		auto = 1
		status = "starting"
	}
	row, err := s.DB.Queries.CreateACPConversation(ctx, sqlcdb.CreateACPConversationParams{ID: req.ID, ProjectID: req.ProjectID, ReleaseID: req.ReleaseID, Automatic: auto, Title: req.Title, ProfileJson: encode(cfg.Harness), Status: status, CreatedAt: now(), UpdatedAt: now()})
	if err == nil {
		s.publish()
	}
	return row, err
}
func (s *Service) Get(ctx context.Context, id string) (sqlcdb.AcpConversation, error) {
	return s.DB.Queries.GetACPConversation(ctx, id)
}
func (s *Service) List(ctx context.Context, project string) ([]sqlcdb.AcpConversation, error) {
	return s.DB.Queries.ListACPConversations(ctx, project)
}
func (s *Service) EventsAfter(ctx context.Context, id string, after int64) ([]sqlcdb.AcpEvent, error) {
	return s.DB.Queries.ListACPEvents(ctx, sqlcdb.ListACPEventsParams{ConversationID: id, ID: after})
}
func (s *Service) Permissions(ctx context.Context) ([]sqlcdb.AcpPermission, error) {
	return s.DB.Queries.ListACPPermissions(ctx)
}
func (s *Service) Title(ctx context.Context, id, title string) error {
	if len([]rune(title)) > 120 {
		return ErrInvalid
	}
	if _, err := s.Get(ctx, id); err != nil {
		return err
	}
	err := s.DB.Queries.SetACPTitle(ctx, sqlcdb.SetACPTitleParams{ID: id, Title: title, UpdatedAt: now()})
	s.publish()
	return err
}
func (s *Service) run(id string, operation func(*runner) (map[string]any, error)) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.ctx.Err() != nil {
		return s.ctx.Err()
	}
	r := s.runners[id]
	if r == nil {
		row, err := s.Get(s.ctx, id)
		if err != nil {
			return err
		}
		r = newRunner(s, row)
		s.runners[id] = r
	}
	if r.active {
		return ErrConflict
	}
	r.mu.Lock()
	r.row.Status = "starting"
	r.cancelled = false
	if err := s.save(s.ctx, r.row); err != nil {
		r.mu.Unlock()
		return err
	}
	r.mu.Unlock()
	r.active = true
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		result, err := operation(r)
		if r.row.Automatic != 0 && s.ctx.Err() == nil {
			s.finishReview(r.row.ReleaseID, err)
		}
		s.mu.Lock()
		r.mu.Lock()
		if err != nil {
			r.row.Status = "failed"
			r.row.Error = err.Error()
		} else {
			r.row.Status = "idle"
			r.row.Error = ""
		}
		if s.ctx.Err() != nil {
			r.row.Status = "interrupted"
		}
		_ = s.completeOperation(context.Background(), r.row, result, err)
		r.active = false
		r.lastUsed = time.Now()
		r.mu.Unlock()
		s.mu.Unlock()
		s.publish()
	}()
	return nil
}
func (s *Service) Initialize(id string) error {
	return s.run(id, func(r *runner) (map[string]any, error) { return nil, r.initialize() })
}
func (s *Service) Prompt(id string, content []map[string]any) error {
	return s.PromptWithDisplay(id, content, nil)
}
func (s *Service) PromptWithDisplay(id string, content []map[string]any, display *string) error {
	if len(content) == 0 {
		return ErrInvalid
	}
	for _, c := range content {
		if c["type"] != "text" && c["type"] != "image" {
			return ErrInvalid
		}
	}
	return s.run(id, func(r *runner) (map[string]any, error) {
		if err := r.initialize(); err != nil {
			return nil, err
		}
		for _, c := range content {
			if c["type"] == "image" && !r.images {
				return nil, fmt.Errorf("agent does not support images")
			}
		}
		r.mu.Lock()
		r.row.Status = "running"
		r.row.Error = ""
		err := s.save(s.ctx, r.row)
		r.mu.Unlock()
		if err != nil {
			return nil, err
		}
		if err := s.append(s.ctx, id, "prompt", map[string]any{"content": content, "display_text": display}); err != nil {
			return nil, err
		}
		result, err := r.call("session/prompt", map[string]any{"sessionId": r.session(), "prompt": content})
		if err != nil {
			return nil, err
		}
		r.mu.Lock()
		if r.cancelled {
			result["stopReason"] = "cancelled"
		}
		r.mu.Unlock()
		return result, nil
	})
}
func (s *Service) ConfigureSession(id, key string, value any) error {
	if key == "" {
		return ErrInvalid
	}
	switch value.(type) {
	case string, bool:
	default:
		return ErrInvalid
	}
	return s.run(id, func(r *runner) (map[string]any, error) {
		if err := r.initialize(); err != nil {
			return nil, err
		}
		if r.codex && key == "mode" && value != "read-only" {
			return nil, ErrInvalid
		}
		params := map[string]any{"sessionId": r.session(), "configId": key, "value": value}
		if _, ok := value.(bool); ok {
			params["type"] = "boolean"
		}
		result, err := r.call("session/set_config_option", params)
		if err != nil {
			return nil, err
		}
		r.options(result)
		return nil, s.append(s.ctx, id, "configured", params)
	})
}
func (s *Service) Authenticate(id, method string) error {
	return s.run(id, func(r *runner) (map[string]any, error) {
		if err := r.handshake(); err != nil {
			return nil, err
		}
		if _, err := r.call("authenticate", map[string]any{"methodId": method}); err != nil {
			return nil, err
		}
		return nil, r.initialize()
	})
}
func (s *Service) Cancel(id string) error {
	s.mu.Lock()
	r := s.runners[id]
	s.mu.Unlock()
	if r == nil {
		return nil
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.cancelled = true
	pending, err := s.DB.Queries.ListACPPermissions(s.ctx)
	if err != nil {
		return err
	}
	if err := s.DB.Queries.CancelACPPermissions(s.ctx, sqlcdb.CancelACPPermissionsParams{ConversationID: id, ResolvedAt: now()}); err != nil {
		return err
	}
	for _, p := range pending {
		if p.ConversationID != id {
			continue
		}
		var req map[string]any
		_ = json.Unmarshal([]byte(p.RequestJson), &req)
		_ = r.send(map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": map[string]any{"outcome": map[string]any{"outcome": "cancelled"}}})
	}
	s.publish()
	err = r.send(map[string]any{"jsonrpc": "2.0", "method": "session/cancel", "params": map[string]any{"sessionId": r.row.SessionID}})
	time.AfterFunc(3*time.Second, func() {
		r.mu.Lock()
		defer r.mu.Unlock()
		if r.cancelled && (r.row.Status == "starting" || r.row.Status == "running" || r.row.Status == "waiting") {
			r.stop()
		}
	})
	return err
}
func (s *Service) reapIdle() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for id, r := range s.runners {
		if !r.active && !r.lastUsed.IsZero() && time.Since(r.lastUsed) > 10*time.Minute {
			r.stop()
			delete(s.runners, id)
		}
	}
}
func (s *Service) ClearGrants(ctx context.Context, id string) error {
	row, err := s.Get(ctx, id)
	if err != nil {
		return err
	}
	return s.DB.Queries.ClearACPGrants(ctx, identity(row.ProfileJson))
}

// Migration reads only configuration on the daemon host; remote client credentials never move.
func (s *Service) MigrateProfile(ctx context.Context, path string) error {
	cfg, err := s.Settings(ctx)
	if err != nil || cfg.Revision != 1 || cfg.Harness != nil {
		return err
	}
	raw, err := os.ReadFile(filepath.Clean(path))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var old struct {
		Harness *Profile `json:"harness"`
	}
	if json.Unmarshal(raw, &old) != nil || old.Harness == nil || old.Harness.Protocol != "acp" {
		return nil
	}
	_, err = s.Configure(ctx, Settings{Revision: cfg.Revision, Harness: old.Harness})
	return err
}
