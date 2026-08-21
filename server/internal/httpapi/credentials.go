package httpapi

import (
	"errors"
	"net/http"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

type credentialStatus struct {
	Name       string `json:"name"`
	Configured bool   `json:"configured"`
	Backend    string `json:"backend"`
}

func (s *Server) listCredentials(w http.ResponseWriter, r *http.Request) {
	rows, err := s.DB.Queries.ListCredentialNames(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	out := make([]credentialStatus, 0, len(rows))
	backend := s.secretBackend()
	for _, row := range rows {
		if secret.IsInternalName(row.Name) {
			continue
		}
		out = append(out, credentialStatus{Name: row.Name, Configured: true, Backend: backend})
	}
	writeJSON(w, http.StatusOK, map[string]any{"credentials": out})
}

func (s *Server) getCredentialStatus(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if err := rejectCredentialName(name); err != nil {
		writeRequestError(w, err)
		return
	}
	configured := false
	exists, err := s.Secrets.Exists(r.Context(), name)
	if err != nil && !errors.Is(err, secret.ErrNotFound) {
		writeRequestError(w, err)
		return
	}
	configured = exists
	writeJSON(w, http.StatusOK, credentialStatus{
		Name:       name,
		Configured: configured,
		Backend:    s.secretBackend(),
	})
}

func (s *Server) putCredential(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if err := rejectCredentialName(name); err != nil {
		writeRequestError(w, err)
		return
	}
	var body struct {
		Value string `json:"value"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	if err := s.Secrets.Set(r.Context(), name, []byte(body.Value)); err != nil {
		writeRequestError(w, err)
		return
	}
	_ = s.DB.Queries.UpsertCredential(r.Context(), sqlcdb.UpsertCredentialParams{
		Name:      name,
		UpdatedAt: time.Now().UTC().Format(time.RFC3339Nano),
	})
	writeJSON(w, http.StatusOK, credentialStatus{
		Name:       name,
		Configured: true,
		Backend:    s.secretBackend(),
	})
}

func (s *Server) deleteCredential(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if err := rejectCredentialName(name); err != nil {
		writeRequestError(w, err)
		return
	}
	if err := s.Secrets.Delete(r.Context(), name); err != nil && !errors.Is(err, secret.ErrNotFound) {
		writeRequestError(w, err)
		return
	}
	_ = s.DB.Queries.DeleteCredentialMeta(r.Context(), name)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) secretBackend() string {
	if s.Secrets == nil {
		return ""
	}
	return s.Secrets.Backend()
}

func rejectCredentialName(name string) error {
	if err := secret.ValidateName(name); err != nil {
		return err
	}
	if secret.IsInternalName(name) {
		return secret.ErrInvalidName
	}
	return nil
}
