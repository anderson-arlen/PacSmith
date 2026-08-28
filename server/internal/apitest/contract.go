package apitest

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/auth"
	"github.com/anderson-arlen/pacsmith/server/internal/daemon"
	githubapi "github.com/anderson-arlen/pacsmith/server/internal/github"
	"github.com/anderson-arlen/pacsmith/server/internal/httpapi"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/legacy"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/paths"
	"github.com/anderson-arlen/pacsmith/server/internal/pki"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/version"
)

func UnixClient(socket string) *http.Client {
	return &http.Client{
		Timeout: 15 * time.Second,
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				var dialer net.Dialer
				return dialer.DialContext(ctx, "unix", socket)
			},
		},
	}
}

func StartDaemon(t *testing.T) (paths.Dirs, *http.Client) {
	t.Helper()
	dirs, client, _ := startDaemon(t, "")
	return dirs, client
}

func StartDaemonTLS(t *testing.T) (paths.Dirs, *http.Client, string) {
	t.Helper()
	dirs, client, d := startDaemon(t, "127.0.0.1:0")
	addr := d.TLSAddr()
	if addr == "" {
		t.Fatal("TLS listener did not start")
	}
	return dirs, client, addr
}

func StartConfigured(t *testing.T, listen string) (paths.Dirs, *http.Client, *daemon.Daemon) {
	t.Helper()
	return startDaemon(t, listen)
}

func startDaemon(t *testing.T, listen string) (paths.Dirs, *http.Client, *daemon.Daemon) {
	t.Helper()
	t.Setenv("PACSMITH_SECRET_BACKEND", "file")
	root := t.TempDir()
	legacyDir := filepath.Join(root, "data", "pacsmith", "projects", "sentinel")
	if err := os.MkdirAll(legacyDir, 0o755); err != nil {
		t.Fatal(err)
	}
	sentinel := []byte("legacy-library-must-not-change\n")
	if err := os.WriteFile(filepath.Join(legacyDir, "project.json"), sentinel, 0o644); err != nil {
		t.Fatal(err)
	}
	dirs, err := paths.Resolve(paths.Overrides{
		DataHome:   filepath.Join(root, "data"),
		ConfigHome: filepath.Join(root, "config"),
		StateHome:  filepath.Join(root, "state"),
		RuntimeDir: filepath.Join(root, "runtime"),
	})
	if err != nil {
		t.Fatal(err)
	}
	d, err := daemon.StartConfig(context.Background(), daemon.Config{Dirs: dirs, Listen: listen})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = d.Close() })
	return dirs, UnixClient(dirs.Socket), d
}

func RunContract(t *testing.T, client *http.Client, origin string) {
	t.Helper()
	t.Run("version", func(t *testing.T) {
		resp := mustDo(t, client, origin, http.MethodGet, "/api/v1/version", nil, nil)
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("version status %d", resp.StatusCode)
		}
		var body struct {
			APIVersion    string   `json:"api_version"`
			ServerVersion string   `json:"server_version"`
			Capabilities  []string `json:"capabilities"`
		}
		decodeJSON(t, resp, &body)
		if body.APIVersion != version.API {
			t.Fatalf("api_version %q", body.APIVersion)
		}
		if body.ServerVersion == "" {
			t.Fatal("missing server_version")
		}
		if !contains(body.Capabilities, "http") || !contains(body.Capabilities, "unix") ||
			!contains(body.Capabilities, "library") || !contains(body.Capabilities, "events") {
			t.Fatalf("capabilities %v", body.Capabilities)
		}
	})
	t.Run("health", func(t *testing.T) {
		resp := mustDo(t, client, origin, http.MethodGet, "/api/v1/health", nil, nil)
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("health status %d", resp.StatusCode)
		}
		var body struct {
			Status   string `json:"status"`
			Database string `json:"database"`
		}
		decodeJSON(t, resp, &body)
		if body.Status != "ok" || body.Database != "ok" {
			t.Fatalf("health %+v", body)
		}
	})
	t.Run("artifact_stream_roundtrip", func(t *testing.T) {
		payload := bytes.Repeat([]byte("pacsmith-artifact-stream\n"), 4096)
		sum := sha256.Sum256(payload)
		want := hex.EncodeToString(sum[:])
		headers := map[string]string{
			"Pacsmith-Filename": "vendor package.bin",
			"Pacsmith-Kind":     "archive",
			"Content-Type":      "application/octet-stream",
		}
		resp := mustDo(t, client, origin, http.MethodPost, "/api/v1/artifacts", headers, payload)
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusCreated {
			t.Fatalf("upload status %d %s", resp.StatusCode, readBody(t, resp))
		}
		var created artifact.Record
		decodeJSON(t, resp, &created)
		if created.ID == "" || created.SHA256 != want || created.SizeBytes != int64(len(payload)) {
			t.Fatalf("created %+v want sha %s size %d", created, want, len(payload))
		}
		if created.OriginalFilename != "vendor package.bin" || created.Kind != "archive" {
			t.Fatalf("metadata %+v", created)
		}

		meta := mustDo(t, client, origin, http.MethodGet, "/api/v1/artifacts/"+created.ID, nil, nil)
		defer meta.Body.Close()
		if meta.StatusCode != http.StatusOK {
			t.Fatalf("meta status %d", meta.StatusCode)
		}

		download := mustDo(t, client, origin, http.MethodGet, "/api/v1/artifacts/"+created.ID+"/content", nil, nil)
		defer download.Body.Close()
		if download.StatusCode != http.StatusOK {
			t.Fatalf("download status %d", download.StatusCode)
		}
		got, err := io.ReadAll(download.Body)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, payload) {
			t.Fatalf("downloaded %d bytes, payload %d bytes", len(got), len(payload))
		}
		if download.Header.Get("Pacsmith-Sha256") != want {
			t.Fatalf("sha header %s", download.Header.Get("Pacsmith-Sha256"))
		}

		again := mustDo(t, client, origin, http.MethodPost, "/api/v1/artifacts", headers, payload)
		defer again.Body.Close()
		var duplicate artifact.Record
		decodeJSON(t, again, &duplicate)
		if duplicate.ID != created.ID {
			t.Fatalf("duplicate ingest created %s want %s", duplicate.ID, created.ID)
		}
	})
	t.Run("rejects_dot_filename", func(t *testing.T) {
		headers := map[string]string{
			"Pacsmith-Filename": "..",
			"Pacsmith-Kind":     "archive",
		}
		resp := mustDo(t, client, origin, http.MethodPost, "/api/v1/artifacts", headers, []byte("x"))
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusBadRequest {
			t.Fatalf("status %d", resp.StatusCode)
		}
	})
	t.Run("library_import_and_files", func(t *testing.T) {
		deb, err := os.ReadFile(AssembleSampleDeb(t))
		if err != nil {
			t.Fatal(err)
		}
		headers := map[string]string{
			"Pacsmith-Filename": "sample.deb",
			"Pacsmith-Kind":     "debian",
			"Content-Type":      "application/octet-stream",
		}
		upload := mustDo(t, client, origin, http.MethodPost, "/api/v1/artifacts", headers, deb)
		defer upload.Body.Close()
		if upload.StatusCode != http.StatusCreated {
			t.Fatalf("upload %d %s", upload.StatusCode, readBody(t, upload))
		}
		var created artifact.Record
		decodeJSON(t, upload, &created)
		req, _ := json.Marshal(map[string]string{"artifact_id": created.ID})
		imported := mustDo(t, client, origin, http.MethodPost, "/api/v1/imports", map[string]string{
			"Content-Type": "application/json",
		}, req)
		defer imported.Body.Close()
		if imported.StatusCode != http.StatusAccepted {
			t.Fatalf("import %d %s", imported.StatusCode, readBody(t, imported))
		}
		var accepted struct {
			JobID string `json:"job_id"`
		}
		decodeJSON(t, imported, &accepted)
		job := waitJob(t, client, origin, accepted.JobID)
		if job.Status != "succeeded" {
			t.Fatalf("job %+v", job)
		}
		list := mustDo(t, client, origin, http.MethodGet, "/api/v1/projects", nil, nil)
		defer list.Body.Close()
		var projects struct {
			Projects []struct {
				ID              string `json:"id"`
				ArchPackageName string `json:"archPackageName"`
				Releases        []struct {
					ID string `json:"id"`
				} `json:"releases"`
			} `json:"projects"`
		}
		decodeJSON(t, list, &projects)
		if len(projects.Projects) != 1 || projects.Projects[0].ArchPackageName != "pacsmith-smoke-bin" {
			t.Fatalf("projects %+v", projects)
		}
		releaseID := projects.Projects[0].Releases[0].ID
		inspectionResp := mustDo(t, client, origin, http.MethodGet,
			"/api/v1/releases/"+releaseID+"/payload-inspection?path="+
				url.QueryEscape("usr/bin/pacsmith-smoke"), nil, nil)
		if inspectionResp.StatusCode != http.StatusOK {
			t.Fatalf("payload inspection %d %s", inspectionResp.StatusCode, readBody(t, inspectionResp))
		}
		var inspection struct {
			Path       string `json:"path"`
			Mode       string `json:"mode"`
			Executable bool   `json:"executable"`
			SHA256     string `json:"sha256"`
			Text       string `json:"text"`
		}
		decodeJSON(t, inspectionResp, &inspection)
		inspectionResp.Body.Close()
		if inspection.Path != "usr/bin/pacsmith-smoke" || inspection.Mode != "0755" ||
			!inspection.Executable || inspection.SHA256 == "" ||
			!strings.Contains(inspection.Text, "PacSmith fixture payload") {
			t.Fatalf("payload inspection %+v", inspection)
		}
		pkgbuild := mustDo(t, client, origin, http.MethodGet, "/api/v1/releases/"+releaseID+"/files/PKGBUILD", nil, nil)
		defer pkgbuild.Body.Close()
		body := readBody(t, pkgbuild)
		if !strings.Contains(body, `pkgname="${_PACSMITH_PKGNAME}"`) ||
			!strings.Contains(body, "depends=('glibc' 'gtk3')") {
			t.Fatalf("pkgbuild %s", body)
		}
		vars := mustDo(t, client, origin, http.MethodGet, "/api/v1/releases/"+releaseID+"/files/pacsmith.vars", nil, nil)
		defer vars.Body.Close()
		if !strings.Contains(readBody(t, vars), "_PACSMITH_PKGNAME='pacsmith-smoke-bin'") {
			t.Fatal("identity vars")
		}
		releaseResp := mustDo(t, client, origin, http.MethodGet, "/api/v1/releases/"+releaseID, nil, nil)
		defer releaseResp.Body.Close()
		if releaseResp.StatusCode != http.StatusOK {
			t.Fatalf("get release %d %s", releaseResp.StatusCode, readBody(t, releaseResp))
		}
		var releaseDoc map[string]any
		decodeJSON(t, releaseResp, &releaseDoc)
		delete(releaseDoc, "identityVariables")
		rev, _ := releaseDoc["revision"].(float64)
		payload, err := json.Marshal(map[string]any{
			"revision": int64(rev),
			"document": releaseDoc,
		})
		if err != nil {
			t.Fatal(err)
		}
		saved := mustDo(t, client, origin, http.MethodPut, "/api/v1/releases/"+releaseID, map[string]string{
			"Content-Type": "application/json",
		}, payload)
		if saved.StatusCode != http.StatusOK {
			t.Fatalf("save release %d %s", saved.StatusCode, readBody(t, saved))
		}
		var savedRelease map[string]any
		decodeJSON(t, saved, &savedRelease)
		saved.Body.Close()
		configurationPatch, err := json.Marshal(map[string]any{
			"revision": savedRelease["revision"],
			"configuration": map[string]any{
				"payloadRules": []map[string]any{{"path": "usr/share/doc/readme", "excluded": true}},
			},
		})
		if err != nil {
			t.Fatal(err)
		}
		patched := mustDo(t, client, origin, http.MethodPatch,
			"/api/v1/releases/"+releaseID+"/configuration",
			map[string]string{"Content-Type": "application/json"}, configurationPatch)
		if patched.StatusCode != http.StatusOK {
			t.Fatalf("patch release configuration %d %s", patched.StatusCode, readBody(t, patched))
		}
		var patchedRelease map[string]any
		decodeJSON(t, patched, &patchedRelease)
		patched.Body.Close()
		forbiddenPatch, _ := json.Marshal(map[string]any{
			"revision":      patchedRelease["revision"],
			"configuration": map[string]any{"payload": []any{}},
		})
		forbidden := mustDo(t, client, origin, http.MethodPatch,
			"/api/v1/releases/"+releaseID+"/configuration",
			map[string]string{"Content-Type": "application/json"}, forbiddenPatch)
		defer forbidden.Body.Close()
		if forbidden.StatusCode != http.StatusBadRequest {
			t.Fatalf("inspection evidence patch status %d %s", forbidden.StatusCode, readBody(t, forbidden))
		}
		varsAfter := mustDo(t, client, origin, http.MethodGet, "/api/v1/releases/"+releaseID+"/files/pacsmith.vars", nil, nil)
		defer varsAfter.Body.Close()
		afterBody := readBody(t, varsAfter)
		for _, snippet := range []string{
			"_PACSMITH_PKGNAME='pacsmith-smoke-bin'",
			"_PACSMITH_ARCH='x86_64'",
			"_PACSMITH_SOURCE=",
			"_PACSMITH_SHA256=",
		} {
			if !strings.Contains(afterBody, snippet) {
				t.Fatalf("identity vars after client-style save missing %q\n%s", snippet, afterBody)
			}
		}
	})
	t.Run("credentials_have_no_readback", func(t *testing.T) {
		put := mustDo(t, client, origin, http.MethodPut, "/api/v1/credentials/github.token", map[string]string{
			"Content-Type": "application/json",
		}, []byte(`{"value":"super-secret-token"}`))
		defer put.Body.Close()
		if put.StatusCode != http.StatusOK {
			t.Fatalf("put %d %s", put.StatusCode, readBody(t, put))
		}
		got := readBody(t, put)
		if strings.Contains(got, "super-secret-token") {
			t.Fatalf("secret leaked: %s", got)
		}
		status := mustDo(t, client, origin, http.MethodGet, "/api/v1/credentials/github.token", nil, nil)
		defer status.Body.Close()
		body := readBody(t, status)
		if strings.Contains(body, "super-secret-token") {
			t.Fatalf("secret leaked on get: %s", body)
		}
		if !strings.Contains(body, `"configured":true`) && !strings.Contains(body, `"configured": true`) {
			t.Fatalf("status %s", body)
		}
	})
	t.Run("library_settings", func(t *testing.T) {
		got := mustDo(t, client, origin, http.MethodGet, "/api/v1/settings", nil, nil)
		defer got.Body.Close()
		if got.StatusCode != http.StatusOK {
			t.Fatalf("get settings %d %s", got.StatusCode, readBody(t, got))
		}
		var settings map[string]any
		decodeJSON(t, got, &settings)
		if _, exists := settings["ai"]; exists {
			t.Fatalf("retired AI settings are still exposed: %+v", settings["ai"])
		}
		settings["updates"] = map[string]any{
			"enabled":               true,
			"daily":                 false,
			"weekday":               3,
			"hour":                  4,
			"minute":                15,
			"automatically_prepare": true,
			"retention_versions":    4,
		}
		settings["build"] = map[string]any{"parallelism": 1}
		payload, err := json.Marshal(settings)
		if err != nil {
			t.Fatal(err)
		}
		patched := mustDo(t, client, origin, http.MethodPatch, "/api/v1/settings", map[string]string{
			"Content-Type": "application/json",
		}, payload)
		defer patched.Body.Close()
		if patched.StatusCode != http.StatusOK {
			t.Fatalf("patch settings %d %s", patched.StatusCode, readBody(t, patched))
		}
		var updated struct {
			Revision int64 `json:"revision"`
			Build    struct {
				Parallelism    int `json:"parallelism"`
				AvailableCores int `json:"available_cores"`
			} `json:"build"`
			Updates struct {
				Enabled           bool `json:"enabled"`
				Hour              int  `json:"hour"`
				Weekday           int  `json:"weekday"`
				RetentionVersions int  `json:"retention_versions"`
			} `json:"updates"`
		}
		decodeJSON(t, patched, &updated)
		if !updated.Updates.Enabled || updated.Updates.Hour != 4 || updated.Updates.Weekday != 3 ||
			updated.Updates.RetentionVersions != 4 || updated.Build.Parallelism != 1 ||
			updated.Build.AvailableCores < 1 {
			t.Fatalf("updated %+v", updated)
		}
		conflict := mustDo(t, client, origin, http.MethodPatch, "/api/v1/settings", map[string]string{
			"Content-Type": "application/json",
		}, payload)
		defer conflict.Body.Close()
		if conflict.StatusCode != http.StatusConflict {
			t.Fatalf("expected revision conflict, got %d %s", conflict.StatusCode, readBody(t, conflict))
		}
	})
	t.Run("event_stream", func(t *testing.T) {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		request, err := http.NewRequestWithContext(ctx, http.MethodGet, origin+"/api/v1/events", nil)
		if err != nil {
			t.Fatal(err)
		}
		stream, err := client.Do(request)
		if err != nil {
			t.Fatal(err)
		}
		defer stream.Body.Close()
		if stream.StatusCode != http.StatusOK || stream.Header.Get("Content-Type") != "text/event-stream" {
			t.Fatalf("event stream status=%d content-type=%q", stream.StatusCode, stream.Header.Get("Content-Type"))
		}
		reader := bufio.NewReader(stream.Body)
		if initial := readSSE(t, reader); !strings.Contains(initial, "event: sync") ||
			!strings.Contains(initial, `"topics":["all"]`) {
			t.Fatalf("unexpected initial event %q", initial)
		}

		settingsResponse := mustDo(t, client, origin, http.MethodGet, "/api/v1/settings", nil, nil)
		var settings map[string]any
		decodeJSON(t, settingsResponse, &settings)
		settingsResponse.Body.Close()
		payload, err := json.Marshal(settings)
		if err != nil {
			t.Fatal(err)
		}
		patched := mustDo(t, client, origin, http.MethodPatch, "/api/v1/settings",
			map[string]string{"Content-Type": "application/json"}, payload)
		if patched.StatusCode != http.StatusOK {
			t.Fatalf("event mutation status %d %s", patched.StatusCode, readBody(t, patched))
		}
		patched.Body.Close()
		if changed := readSSE(t, reader); !strings.Contains(changed, "event: change") ||
			!strings.Contains(changed, `"topics":["settings"]`) {
			t.Fatalf("unexpected change event %q", changed)
		}
	})
	t.Run("builtin_ai_api_is_gone", func(t *testing.T) {
		for _, path := range []string{"/api/v1/ai/models", "/api/v1/ai/github-asset-rule"} {
			response := mustDo(t, client, origin, http.MethodGet, path, nil, nil)
			response.Body.Close()
			if response.StatusCode != http.StatusNotFound && response.StatusCode != http.StatusMethodNotAllowed {
				t.Fatalf("retired endpoint %s returned %d", path, response.StatusCode)
			}
		}
		for _, name := range []string{"openai.api_key", "xai.api_key", "chatgpt.session"} {
			response := mustDo(t, client, origin, http.MethodPut, "/api/v1/credentials/"+name,
				map[string]string{"Content-Type": "application/json"}, []byte(`{"value":"retired"}`))
			response.Body.Close()
			if response.StatusCode != http.StatusBadRequest {
				t.Fatalf("retired credential %s returned %d", name, response.StatusCode)
			}
		}
	})
}

func readSSE(t *testing.T, reader *bufio.Reader) string {
	t.Helper()
	var block strings.Builder
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		block.WriteString(line)
		if line == "\n" || line == "\r\n" {
			return block.String()
		}
	}
}

func AssertLegacyUntouched(t *testing.T, dataHome string, before map[string]string) {
	t.Helper()
	after := TreeFingerprint(t, legacy.ProjectsDir(dataHome))
	if len(after) != len(before) {
		t.Fatalf("legacy tree changed entries %d -> %d", len(before), len(after))
	}
	for path, sum := range before {
		if after[path] != sum {
			t.Fatalf("legacy path %s changed", path)
		}
	}
}

func TreeFingerprint(t *testing.T, root string) map[string]string {
	t.Helper()
	result := map[string]string{}
	err := filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		if info.IsDir() {
			result[rel+"/"] = fmt.Sprintf("dir:%04o", info.Mode().Perm())
			return nil
		}
		body, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		sum := sha256.Sum256(body)
		result[rel] = fmt.Sprintf("%s:%04o", hex.EncodeToString(sum[:]), info.Mode().Perm())
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return result
}

func NewHandler(t *testing.T) (http.Handler, paths.Dirs) {
	t.Helper()
	t.Setenv("PACSMITH_SECRET_BACKEND", "file")
	root := t.TempDir()
	dirs, err := paths.Resolve(paths.Overrides{
		DataHome:   filepath.Join(root, "data"),
		ConfigHome: filepath.Join(root, "config"),
		StateHome:  filepath.Join(root, "state"),
		RuntimeDir: filepath.Join(root, "runtime"),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := dirs.Ensure(); err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	db, err := sqlite.Open(ctx, dirs.Database)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	opened, err := secret.Open(ctx, db, dirs.Config)
	if err != nil {
		t.Fatal(err)
	}
	runtime, err := pki.LoadOrGenerate(ctx, db, opened.Store)
	if err != nil {
		t.Fatal(err)
	}
	store, err := artifact.New(dirs.Objects, dirs.Tmp)
	if err != nil {
		t.Fatal(err)
	}
	registry := &artifact.Registry{DB: db, Store: store}
	lib := &library.Service{DB: db, Artifacts: registry, WorkDir: filepath.Join(dirs.Work, "releases")}
	repoSvc := repo.New(db, registry, opened.Store, filepath.Join(dirs.Work, "repo"), filepath.Join(dirs.Data, "gnupg"))
	lib.Repo = repoSvc
	githubSvc := &githubapi.Service{Secrets: opened.Store, Artifacts: registry}
	manager, err := jobs.New(db, filepath.Join(dirs.Work, "jobs"), daemon.JobHandler(lib, githubSvc))
	if err != nil {
		t.Fatal(err)
	}
	if err := manager.Start(ctx); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Stop)
	return httpapi.New(httpapi.Config{
		DB:        db,
		Artifacts: registry,
		Library:   lib,
		Jobs:      manager,
		Secrets:   opened.Store,
		PKI:       runtime,
		Principal: auth.LocalUnix(),
		Repo:      repoSvc,
		GitHub:    githubSvc,
	}), dirs
}

func mustDo(t *testing.T, client *http.Client, origin, method, path string, headers map[string]string, body []byte) *http.Response {
	t.Helper()
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	req, err := http.NewRequest(method, origin+path, reader)
	if err != nil {
		t.Fatal(err)
	}
	for key, value := range headers {
		req.Header.Set(key, value)
	}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	return resp
}

func waitJob(t *testing.T, client *http.Client, origin, id string) struct {
	ID     string `json:"id"`
	Status string `json:"status"`
	Error  string `json:"error"`
} {
	t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	var job struct {
		ID     string `json:"id"`
		Status string `json:"status"`
		Error  string `json:"error"`
	}
	for time.Now().Before(deadline) {
		resp := mustDo(t, client, origin, http.MethodGet, "/api/v1/jobs/"+id, nil, nil)
		if resp.StatusCode != http.StatusOK {
			body := readBody(t, resp)
			resp.Body.Close()
			t.Fatalf("job status %d %s", resp.StatusCode, body)
		}
		decodeJSON(t, resp, &job)
		resp.Body.Close()
		if job.Status == "succeeded" || job.Status == "failed" || job.Status == "interrupted" {
			return job
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("job %s did not finish: %+v", id, job)
	return job
}

func decodeJSON(t *testing.T, resp *http.Response, dest any) {
	t.Helper()
	if err := json.NewDecoder(resp.Body).Decode(dest); err != nil {
		t.Fatal(err)
	}
}

func readBody(t *testing.T, resp *http.Response) string {
	t.Helper()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return string(body)
}

func contains(values []string, want string) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}
