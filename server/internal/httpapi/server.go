package httpapi

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/auth"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/listen"
	"github.com/anderson-arlen/pacsmith/server/internal/pki"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/version"
)

const (
	readHeaderTimeout = 10 * time.Second
	idleTimeout       = 2 * time.Minute
	jsonBodyLimit     = 1 << 20
	registerBodyLimit = 32 << 10
)

type Config struct {
	DB          *sqlite.DB
	Artifacts   *artifact.Registry
	Library     *library.Service
	Jobs        *jobs.Manager
	Secrets     *secret.LockedStore
	PKI         *pki.Runtime
	Principal   auth.Principal
	Listen      *listen.State
	ApplyListen func(listen.Config) error
	Repo        *repo.Service
	ApplyRepo   func(listen.Config) error
	RepoBound   func() []string
}

type Server struct {
	Config
	limiter *ipLimiter
}

func New(cfg Config) http.Handler {
	server := &Server{Config: cfg, limiter: newIPLimiter()}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/version", server.version)
	mux.HandleFunc("GET /api/v1/health", server.health)
	mux.HandleFunc("POST /api/v1/artifacts", server.createArtifact)
	mux.HandleFunc("GET /api/v1/artifacts/{id}", server.getArtifact)
	mux.HandleFunc("GET /api/v1/artifacts/{id}/content", server.getArtifactContent)
	mux.HandleFunc("GET /api/v1/projects", server.listProjects)
	mux.HandleFunc("GET /api/v1/projects/{id}", server.getProject)
	mux.HandleFunc("PATCH /api/v1/projects/{id}", server.patchProject)
	mux.HandleFunc("DELETE /api/v1/projects/{id}", server.deleteProject)
	mux.HandleFunc("GET /api/v1/releases/{id}", server.getRelease)
	mux.HandleFunc("PUT /api/v1/releases/{id}", server.putRelease)
	mux.HandleFunc("DELETE /api/v1/releases/{id}", server.deleteRelease)
	mux.HandleFunc("GET /api/v1/releases/{id}/files/{name}", server.getReleaseFile)
	mux.HandleFunc("PUT /api/v1/releases/{id}/files/{name}", server.putReleaseFile)
	mux.HandleFunc("PUT /api/v1/releases/{id}/icon", server.putReleaseIcon)
	mux.HandleFunc("POST /api/v1/imports", server.createImport)
	mux.HandleFunc("POST /api/v1/releases/{id}/reanalyze", server.reanalyze)
	mux.HandleFunc("POST /api/v1/releases/{id}/builds", server.createBuild)
	mux.HandleFunc("POST /api/v1/releases/{id}/ai", server.createPackageAI)
	mux.HandleFunc("POST /api/v1/ai/github-asset-rule", server.createGitHubAssetAI)
	mux.HandleFunc("GET /api/v1/ai/models", server.listAIModels)
	mux.HandleFunc("GET /api/v1/jobs/{id}", server.getJob)
	mux.HandleFunc("GET /api/v1/jobs/{id}/log", server.getJobLog)
	mux.HandleFunc("POST /api/v1/jobs/{id}/cancel", server.cancelJob)
	mux.HandleFunc("GET /api/v1/credentials", server.listCredentials)
	mux.HandleFunc("GET /api/v1/credentials/{name}", server.getCredentialStatus)
	mux.HandleFunc("PUT /api/v1/credentials/{name}", server.putCredential)
	mux.HandleFunc("DELETE /api/v1/credentials/{name}", server.deleteCredential)
	mux.HandleFunc("POST /api/v1/registrations", server.createRegistration)
	mux.HandleFunc("GET /api/v1/registrations/{id}", server.getRegistration)
	mux.HandleFunc("GET /api/v1/registrations", server.listRegistrations)
	mux.HandleFunc("POST /api/v1/registrations/{id}/approve", server.approveRegistration)
	mux.HandleFunc("POST /api/v1/registrations/{id}/reject", server.rejectRegistration)
	mux.HandleFunc("GET /api/v1/clients", server.listClients)
	mux.HandleFunc("POST /api/v1/clients/{id}/revoke", server.revokeClient)
	mux.HandleFunc("GET /api/v1/server", server.serverInfo)
	mux.HandleFunc("PATCH /api/v1/server", server.patchServer)
	mux.HandleFunc("GET /api/v1/settings", server.getSettings)
	mux.HandleFunc("PATCH /api/v1/settings", server.patchSettings)
	mux.HandleFunc("GET /api/v1/repo", server.getRepo)
	mux.HandleFunc("PATCH /api/v1/repo", server.patchRepo)
	mux.HandleFunc("POST /api/v1/repo/signing/init", server.initRepoSigning)
	mux.HandleFunc("GET /api/v1/repo/signing/public-key", server.getRepoPublicKey)
	mux.HandleFunc("POST /api/v1/repo/signing/root", server.uploadRepoRoot)
	mux.HandleFunc("POST /api/v1/repo/signing/certified", server.uploadRepoCertified)
	mux.HandleFunc("GET /api/v1/repo/bootstrap", server.getRepoBootstrap)
	mux.HandleFunc("GET /api/v1/projects/{id}/repo", server.getProjectRepo)
	mux.HandleFunc("PATCH /api/v1/projects/{id}/repo", server.patchProjectRepo)
	mux.HandleFunc("POST /api/v1/projects/{id}/repo/promote", server.promoteProjectRepo)
	mux.HandleFunc("POST /api/v1/cleanup", server.runCleanup)
	return server.middleware(mux)
}

func NewHTTPServer(handler http.Handler) *http.Server {
	return &http.Server{
		Handler:           handler,
		ReadHeaderTimeout: readHeaderTimeout,
		IdleTimeout:       idleTimeout,
	}
}

func (s *Server) middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		principal := s.principalFor(r)
		if !authorized(principal, r) {
			writeError(w, http.StatusForbidden, "forbidden", "not authorized")
			return
		}
		next.ServeHTTP(w, r.WithContext(auth.WithPrincipal(r.Context(), principal)))
	})
}

func (s *Server) principalFor(r *http.Request) auth.Principal {
	if r.TLS == nil {
		return s.Principal
	}
	if len(r.TLS.PeerCertificates) == 0 {
		return auth.Principal{Kind: auth.KindEnrollment}
	}
	fingerprint := pki.CertSHA256(r.TLS.PeerCertificates[0])
	client, err := s.DB.Queries.GetClientByCertSHA256(r.Context(), fingerprint)
	if err != nil || client.Revoked != 0 {
		return auth.Principal{Kind: auth.KindEnrollment}
	}
	return auth.Principal{Kind: auth.KindRemoteClient, ClientID: client.ID}
}

func authorized(principal auth.Principal, r *http.Request) bool {
	path := r.URL.Path
	switch {
	case path == "/api/v1/version" || path == "/api/v1/health":
		return true
	case r.Method == http.MethodPost && path == "/api/v1/registrations":
		return true
	case r.Method == http.MethodGet && strings.HasPrefix(path, "/api/v1/registrations/"):
		return true
	case isLocalAdminPath(path):
		return principal.IsLocalAdmin()
	default:
		return principal.IsLocalAdmin() || principal.Kind == auth.KindRemoteClient
	}
}

func isLocalAdminPath(path string) bool {
	if path == "/api/v1/server" || path == "/api/v1/clients" || path == "/api/v1/registrations" {
		return true
	}
	if strings.HasPrefix(path, "/api/v1/clients/") {
		return true
	}
	if strings.HasSuffix(path, "/approve") || strings.HasSuffix(path, "/reject") {
		return true
	}
	return false
}

type versionResponse struct {
	APIVersion    string   `json:"api_version"`
	ServerVersion string   `json:"server_version"`
	Capabilities  []string `json:"capabilities"`
}

func (s *Server) version(w http.ResponseWriter, _ *http.Request) {
	caps := append([]string(nil), version.Capabilities...)
	if s.Listen != nil && s.Listen.Serving() {
		caps = append(caps, "tls")
	}
	writeJSON(w, http.StatusOK, versionResponse{
		APIVersion:    version.API,
		ServerVersion: version.Version,
		Capabilities:  caps,
	})
}

type healthResponse struct {
	Status   string `json:"status"`
	Database string `json:"database"`
}

func (s *Server) health(w http.ResponseWriter, r *http.Request) {
	status := healthResponse{Status: "ok", Database: "ok"}
	if err := s.DB.Ping(r.Context()); err != nil {
		status.Status = "unhealthy"
		status.Database = "error"
		writeJSON(w, http.StatusServiceUnavailable, status)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

type apiError struct {
	Error errorBody `json:"error"`
}

type errorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	body, err := json.Marshal(value)
	if err != nil {
		http.Error(w, `{"error":{"code":"internal","message":"internal error"}}`, http.StatusInternalServerError)
		return
	}
	body = append(body, '\n')
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(status)
	_, _ = w.Write(body)
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, apiError{Error: errorBody{Code: code, Message: message}})
}

func writeRequestError(w http.ResponseWriter, err error) {
	if err == nil {
		return
	}
	message := err.Error()
	switch {
	case errors.Is(err, artifact.ErrNotFound), errors.Is(err, library.ErrNotFound), errors.Is(err, jobs.ErrNotFound), errors.Is(err, secret.ErrNotFound), errors.Is(err, repo.ErrNotFound):
		writeError(w, http.StatusNotFound, "not_found", message)
	case errors.Is(err, library.ErrConflict), errors.Is(err, repo.ErrConflict):
		writeError(w, http.StatusConflict, "conflict", message)
	case errors.Is(err, library.ErrInvalid), errors.Is(err, secret.ErrInvalidName), errors.Is(err, secret.ErrReadOnly), errors.Is(err, repo.ErrInvalid):
		writeError(w, http.StatusBadRequest, "bad_request", message)
	case errors.Is(err, secret.ErrUnavailable):
		writeError(w, http.StatusServiceUnavailable, "unavailable", message)
	case isBadRequest(message):
		writeError(w, http.StatusBadRequest, "bad_request", message)
	default:
		writeError(w, http.StatusInternalServerError, "internal", "internal error")
	}
}

func isBadRequest(message string) bool {
	return strings.Contains(message, "filename") ||
		strings.Contains(message, "kind is invalid") ||
		strings.Contains(message, "invalid artifact") ||
		strings.Contains(message, "missing artifact") ||
		strings.Contains(message, "invalid request")
}

func decodeJSON(w http.ResponseWriter, r *http.Request, dest any) bool {
	return decodeJSONLimit(w, r, dest, jsonBodyLimit)
}

func decodeJSONLimit(w http.ResponseWriter, r *http.Request, dest any, limit int64) bool {
	defer r.Body.Close()
	limited := io.LimitReader(r.Body, limit+1)
	body, err := io.ReadAll(limited)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "could not read body")
		return false
	}
	if int64(len(body)) > limit {
		writeError(w, http.StatusRequestEntityTooLarge, "too_large", "request body is too large")
		return false
	}
	if err := json.Unmarshal(body, dest); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "invalid json")
		return false
	}
	return true
}
