package acp

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

type rpcReply struct {
	Result map[string]any `json:"result"`
	Error  *struct {
		Message string `json:"message"`
	} `json:"error"`
}
type runner struct {
	service                               *Service
	mu                                    sync.Mutex
	row                                   sqlcdb.AcpConversation
	profile                               Profile
	active                                bool
	ready, images, load, codex, replaying bool
	initialized                           bool
	cancelled                             bool
	lastUsed                              time.Time
	ioMu                                  sync.Mutex
	cmd                                   *exec.Cmd
	input                                 io.WriteCloser
	done                                  chan struct{}
	pending                               map[string]chan rpcReply
	next                                  int64
	tools                                 map[string]map[string]any
}

func newRunner(s *Service, row sqlcdb.AcpConversation) *runner {
	r := &runner{service: s, row: row, pending: map[string]chan rpcReply{}, tools: map[string]map[string]any{}}
	_ = json.Unmarshal([]byte(row.ProfileJson), &r.profile)
	pattern := regexp.MustCompile(`(^|[/\\])codex-acp(?:@[^/]+|-(?:x64|arm64)-(?:linux|darwin|windows))?(?:\.exe)?$`)
	for _, part := range append([]string{r.profile.Executable}, r.profile.Arguments...) {
		if pattern.MatchString(part) {
			r.codex = true
		}
	}
	return r
}
func (r *runner) session() string { r.mu.Lock(); defer r.mu.Unlock(); return r.row.SessionID }
func (r *runner) stop() {
	r.ioMu.Lock()
	defer r.ioMu.Unlock()
	if r.cmd != nil && r.cmd.Process != nil {
		_ = r.cmd.Process.Kill()
	}
}
func (r *runner) launch() error {
	if r.codex {
		for _, arg := range r.profile.Arguments {
			if strings.Contains(arg, "sqlite_home") || strings.Contains(arg, "log_dir") || strings.Contains(arg, "--profile") {
				return fmt.Errorf("Codex storage and profiles are managed by the server")
			}
		}
	}

	r.ioMu.Lock()
	defer r.ioMu.Unlock()
	if r.cmd != nil {
		select {
		case <-r.done:
			r.ready = false
			r.initialized = false
		default:
			return nil
		}
	}
	workspace := filepath.Join(r.service.Directory, "workspace")
	if err := os.MkdirAll(workspace, 0700); err != nil {
		return err
	}
	env := map[string]string{}
	for _, entry := range os.Environ() {
		k, v, ok := strings.Cut(entry, "=")
		if ok {
			env[k] = v
		}
	}
	for k, v := range r.profile.Environment {
		env[k] = v
	}
	home := filepath.Join(r.service.Directory, "codex-home")
	if info, err := os.Lstat(home); err == nil && info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("private agent home must not be a symlink")
	}
	if err := os.MkdirAll(home, 0700); err != nil {
		return err
	}
	source := os.Getenv("CODEX_HOME")
	if source == "" {
		source = filepath.Join(os.Getenv("HOME"), ".codex")
	}
	if _, err := os.Stat(filepath.Join(source, "auth.json")); err == nil {
		target := filepath.Join(home, "auth.json")
		if _, err := os.Lstat(target); os.IsNotExist(err) {
			if err := os.Symlink(filepath.Join(source, "auth.json"), target); err != nil {
				return err
			}
		}
	}
	env["CODEX_HOME"] = home
	env["CODEX_SQLITE_HOME"] = home
	env["CODEX_CONFIG"] = encode(map[string]any{"sqlite_home": home, "log_dir": filepath.Join(home, "logs"), "approval_policy": "on-request", "approvals_reviewer": "user"})
	env["APP_SERVER_LOGS"] = filepath.Join(home, "logs")
	env["INITIAL_AGENT_MODE"] = "read-only"
	delete(env, "CODEX_THREAD_ID")
	delete(env, "CODEX_SESSION_ID")
	cmd := exec.CommandContext(r.service.ctx, r.profile.Executable, r.profile.Arguments...)
	cmd.Dir = workspace
	for k, v := range env {
		cmd.Env = append(cmd.Env, k+"="+v)
	}
	input, err := cmd.StdinPipe()
	if err != nil {
		return err
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	// Agent diagnostics stay on the server; bounded RPC errors are returned to clients.
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("start server ACP agent: %w", err)
	}
	r.cmd = cmd
	r.input = input
	r.done = make(chan struct{})
	done := r.done
	r.service.wg.Add(1)
	go func() {
		defer r.service.wg.Done()
		scanner := bufio.NewScanner(output)
		scanner.Buffer(make([]byte, 4096), 16<<20)
		for scanner.Scan() {
			if err := r.receive(scanner.Bytes()); err != nil {
				_ = cmd.Process.Kill()
				break
			}
		}
		_ = cmd.Wait()
		close(done)
	}()
	return nil
}
func (r *runner) send(message any) error {
	r.ioMu.Lock()
	defer r.ioMu.Unlock()
	if r.input == nil {
		return fmt.Errorf("agent is not connected")
	}
	_, err := io.WriteString(r.input, encode(message)+"\n")
	return err
}
func (r *runner) call(method string, params any) (map[string]any, error) {
	r.ioMu.Lock()
	r.next++
	id := strconv.FormatInt(r.next, 10)
	ch := make(chan rpcReply, 1)
	r.pending[id] = ch
	done := r.done
	r.ioMu.Unlock()
	defer func() { r.ioMu.Lock(); delete(r.pending, id); r.ioMu.Unlock() }()
	if err := r.send(map[string]any{"jsonrpc": "2.0", "id": id, "method": method, "params": params}); err != nil {
		return nil, err
	}
	var deadline <-chan time.Time
	if method != "session/prompt" {
		timer := time.NewTimer(time.Minute)
		defer timer.Stop()
		deadline = timer.C
	}
	select {
	case <-r.service.ctx.Done():
		return nil, r.service.ctx.Err()
	case <-done:
		return nil, fmt.Errorf("server ACP agent exited")
	case <-deadline:
		r.stop()
		return nil, fmt.Errorf("ACP request timed out: %s", method)
	case response := <-ch:
		if response.Error != nil {
			return nil, fmt.Errorf("%s", response.Error.Message)
		}
		return response.Result, nil
	}
}
func (r *runner) handshake() error {
	if err := r.launch(); err != nil {
		return err
	}
	if r.initialized {
		return nil
	}

	init, err := r.call("initialize", map[string]any{"protocolVersion": 1, "clientInfo": map[string]any{"name": "pacsmithd", "version": "1"}, "clientCapabilities": map[string]any{"session": map[string]any{"configOptions": map[string]any{"boolean": map[string]any{}}}}})
	if err != nil {
		return err
	}
	if init["protocolVersion"] != float64(1) {
		return fmt.Errorf("agent does not support ACP protocol 1")
	}
	caps := object(init["agentCapabilities"])
	r.load = caps["loadSession"] == true
	r.images = object(caps["promptCapabilities"])["image"] == true
	_ = r.service.append(r.service.ctx, r.row.ID, "authentication", map[string]any{"methods": init["authMethods"]})
	r.initialized = true
	return nil
}
func (r *runner) initialize() error {
	if err := r.handshake(); err != nil {
		return err
	}
	if r.ready {
		return nil
	}
	r.mu.Lock()
	r.row.Status = "starting"
	err := r.service.save(r.service.ctx, r.row)
	r.mu.Unlock()
	if err != nil {
		return err
	}

	method := "session/new"
	params := map[string]any{"cwd": filepath.Join(r.service.Directory, "workspace"), "mcpServers": r.mcpServers()}
	if session := r.session(); session != "" {
		if !r.load {
			return fmt.Errorf("agent cannot resume sessions; start a new conversation")
		}
		method = "session/load"
		params["sessionId"] = session
	}
	r.mu.Lock()
	r.replaying = method == "session/load"
	r.mu.Unlock()
	result, err := r.call(method, params)
	r.mu.Lock()
	r.replaying = false
	if err == nil && method == "session/new" {
		r.row.SessionID = str(result["sessionId"])
	}
	r.mu.Unlock()
	if err != nil {
		return err
	}
	if r.session() == "" {
		return fmt.Errorf("agent returned no session ID")
	}
	if r.codex {
		opts, _ := result["configOptions"].([]any)
		supported := false
		selected := false
		for _, v := range opts {
			o := object(v)
			if o["id"] == "mode" {
				supported = accepts(o["options"], "read-only")
				selected = o["currentValue"] == "read-only"
			}
		}
		if !supported {
			return fmt.Errorf("Codex ACP must support Ask for approval mode")
		}
		if !selected {
			result, err = r.call("session/set_config_option", map[string]any{"sessionId": r.session(), "configId": "mode", "value": "read-only"})
			if err != nil {
				return err
			}
		}
	}
	if r.codex {
		selected := false
		opts, _ := result["configOptions"].([]any)
		for _, entry := range opts {
			o := object(entry)
			if o["id"] == "mode" && o["currentValue"] == "read-only" {
				selected = true
			}
		}
		if !selected {
			return fmt.Errorf("agent did not accept Ask for approval mode")
		}
	}
	defaults := map[string]any{}
	if method == "session/new" {
		for k, v := range r.profile.Defaults {
			defaults[k] = v
		}
	}
	for len(defaults) > 0 {
		applied := false
		opts, _ := result["configOptions"].([]any)
		for _, v := range opts {
			o := object(v)
			key := str(o["id"])
			value, ok := defaults[key]
			if !ok {
				continue
			}
			if r.codex && key == "mode" {
				delete(defaults, key)
				continue
			}
			_, boolean := value.(bool)
			if !(o["type"] == "boolean" && boolean) && !accepts(o["options"], value) {
				continue
			}
			delete(defaults, key)
			p := map[string]any{"sessionId": r.session(), "configId": key, "value": value}
			if boolean {
				p["type"] = "boolean"
			}
			result, err = r.call("session/set_config_option", p)
			if err != nil {
				return err
			}
			applied = true
			break
		}
		if !applied {
			break
		}
	}
	r.options(result)
	r.ready = true
	return nil
}
func accepts(raw, value any) bool {
	values, _ := raw.([]any)
	for _, v := range values {
		o := object(v)
		if o["value"] == value || accepts(o["options"], value) {
			return true
		}
	}
	return false
}
func (r *runner) options(result map[string]any) {
	r.mu.Lock()
	defer r.mu.Unlock()
	opts := result["configOptions"]
	if opts == nil {
		opts = []any{}
	}
	r.row.ConfigJson = encode(opts)
	_ = r.service.save(r.service.ctx, r.row)
}
func (r *runner) mcpServers() []any {
	cli := r.service.CLI
	if cli == "" {
		executable, _ := os.Executable()
		cli = filepath.Join(filepath.Dir(executable), "pacsmith")
	}
	return []any{map[string]any{"name": "pacsmith_session", "command": cli, "args": []string{"mcp"}, "env": []any{
		map[string]string{"name": "PACSMITH_MCP_MODE", "value": "local"}, map[string]string{"name": "PACSMITH_MCP_SOCKET", "value": r.service.Socket},
		map[string]string{"name": "PACSMITH_CONVERSATION_KEY", "value": r.row.ID}, map[string]string{"name": "PACSMITH_SERVER_ACP", "value": "1"},
	}}}
}
func (r *runner) receive(raw []byte) error {
	var message map[string]any
	if err := json.Unmarshal(raw, &message); err != nil {
		return err
	}
	method := str(message["method"])
	id, hasID := message["id"]
	if method == "" {
		r.ioMu.Lock()
		ch := r.pending[str(id)]
		r.ioMu.Unlock()
		if ch != nil {
			var reply rpcReply
			_ = json.Unmarshal(raw, &reply)
			select {
			case ch <- reply:
			default:
			}
		}
		return nil
	}
	params := object(message["params"])
	if method == "session/update" && !hasID {
		r.mu.Lock()
		defer r.mu.Unlock()
		if params["sessionId"] != r.row.SessionID || r.row.SessionID == "" || r.replaying {
			return nil
		}
		update := object(params["update"])
		kind := str(update["sessionUpdate"])
		tool := str(update["toolCallId"])
		if (kind == "tool_call" || kind == "tool_call_update") && tool != "" {
			merged := r.tools[tool]
			if merged == nil {
				merged = map[string]any{}
			}
			for k, v := range update {
				merged[k] = v
			}
			r.tools[tool] = merged
		}
		if kind == "config_option_update" {
			r.row.ConfigJson = encode(update["configOptions"])
			_ = r.service.save(r.service.ctx, r.row)
		}
		return r.service.append(r.service.ctx, r.row.ID, "update", update)
	}
	if method == "session/request_permission" && hasID {
		return r.permission(id, params)
	}
	if hasID {
		return r.send(map[string]any{"jsonrpc": "2.0", "id": id, "error": map[string]any{"code": -32601, "message": "Client capability not supported"}})
	}
	return nil
}
