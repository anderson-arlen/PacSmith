package httpapi

import (
	"crypto/x509"
	"database/sql"
	"encoding/pem"
	"errors"
	"net/http"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"

	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/listen"
	"github.com/anderson-arlen/pacsmith/server/internal/pki"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

type registrationResponse struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	Status   string `json:"status"`
	ClientID string `json:"client_id,omitempty"`
	CertPEM  string `json:"cert_pem,omitempty"`
}

func (s *Server) createRegistration(w http.ResponseWriter, r *http.Request) {
	ip := requestIP(r.RemoteAddr)
	if r.TLS != nil && !s.limiter.allow(ip) {
		writeError(w, http.StatusTooManyRequests, "rate_limited", "too many registration attempts")
		return
	}
	var body struct {
		Name string `json:"name"`
		CSR  string `json:"csr"`
	}
	if !decodeJSONLimit(w, r, &body, registerBodyLimit) {
		return
	}
	name := strings.TrimSpace(body.Name)
	if err := validateFriendlyName(name); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	if err := pki.ValidateCSR([]byte(body.CSR)); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "invalid CSR")
		return
	}
	pending, err := s.DB.Queries.CountPendingRegistrations(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	if pending >= pendingCap {
		writeError(w, http.StatusTooManyRequests, "rate_limited", "too many pending registrations")
		return
	}
	now := time.Now().UTC()
	row, err := s.DB.Queries.InsertRegistration(r.Context(), sqlcdb.InsertRegistrationParams{
		ID:         uuid.NewString(),
		Name:       name,
		Status:     "pending",
		CsrPem:     body.CSR,
		CertPem:    "",
		ClientID:   sql.NullString{},
		CreatedAt:  now.Format(time.RFC3339Nano),
		ExpiresAt:  now.Add(registrationTTL).Format(time.RFC3339Nano),
		RemoteAddr: ip,
	})
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusAccepted, registrationFromRow(row))
}

func (s *Server) getRegistration(w http.ResponseWriter, r *http.Request) {
	row, err := s.expireRegistration(r, r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, registrationFromRow(row))
}

func (s *Server) listRegistrations(w http.ResponseWriter, r *http.Request) {
	rows, err := s.DB.Queries.ListPendingRegistrations(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	out := make([]registrationResponse, 0, len(rows))
	for _, row := range rows {
		updated, err := s.expireRegistrationRow(r, row)
		if err != nil {
			writeRequestError(w, err)
			return
		}
		if updated.Status != "pending" {
			continue
		}
		out = append(out, registrationFromRow(updated))
	}
	writeJSON(w, http.StatusOK, map[string]any{"registrations": out})
}

func (s *Server) approveRegistration(w http.ResponseWriter, r *http.Request) {
	row, err := s.expireRegistration(r, r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	if row.Status != "pending" {
		writeError(w, http.StatusConflict, "conflict", "registration is not pending")
		return
	}
	clientID := uuid.NewString()
	certPEM, err := s.PKI.SignCSR([]byte(row.CsrPem), clientID)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "could not sign CSR")
		return
	}
	block, _ := pem.Decode(certPEM)
	if block == nil {
		writeError(w, http.StatusInternalServerError, "internal", "internal error")
		return
	}
	cert, err := x509.ParseCertificate(block.Bytes)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal", "internal error")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err := s.DB.Queries.InsertClient(r.Context(), sqlcdb.InsertClientParams{
		ID:         clientID,
		Name:       row.Name,
		CertPem:    string(certPEM),
		CertSha256: pki.CertSHA256(cert),
		CreatedAt:  now,
	}); err != nil {
		writeRequestError(w, err)
		return
	}
	updated, err := s.DB.Queries.UpdateRegistration(r.Context(), sqlcdb.UpdateRegistrationParams{
		Status:   "approved",
		CertPem:  string(certPEM),
		ClientID: sql.NullString{String: clientID, Valid: true},
		ID:       row.ID,
	})
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, registrationFromRow(updated))
}

func (s *Server) rejectRegistration(w http.ResponseWriter, r *http.Request) {
	row, err := s.expireRegistration(r, r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	if row.Status != "pending" {
		writeError(w, http.StatusConflict, "conflict", "registration is not pending")
		return
	}
	updated, err := s.DB.Queries.UpdateRegistration(r.Context(), sqlcdb.UpdateRegistrationParams{
		Status:   "rejected",
		CertPem:  row.CertPem,
		ClientID: row.ClientID,
		ID:       row.ID,
	})
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, registrationFromRow(updated))
}

func (s *Server) listClients(w http.ResponseWriter, r *http.Request) {
	rows, err := s.DB.Queries.ListClients(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	type clientJSON struct {
		ID         string `json:"id"`
		Name       string `json:"name"`
		CertSHA256 string `json:"cert_sha256"`
		Revoked    bool   `json:"revoked"`
	}
	out := make([]clientJSON, 0, len(rows))
	for _, row := range rows {
		out = append(out, clientJSON{
			ID:         row.ID,
			Name:       row.Name,
			CertSHA256: row.CertSha256,
			Revoked:    row.Revoked != 0,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"clients": out})
}

func (s *Server) revokeClient(w http.ResponseWriter, r *http.Request) {
	row, err := s.DB.Queries.RevokeClient(r.Context(), r.PathValue("id"))
	if errors.Is(err, sql.ErrNoRows) {
		writeError(w, http.StatusNotFound, "not_found", "client not found")
		return
	}
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"id":          row.ID,
		"name":        row.Name,
		"cert_sha256": row.CertSha256,
		"revoked":     true,
	})
}

func (s *Server) serverInfo(w http.ResponseWriter, _ *http.Request) {
	if s.PKI == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "PKI is not ready")
		return
	}
	writeJSON(w, http.StatusOK, s.serverPayload())
}

func (s *Server) patchServer(w http.ResponseWriter, r *http.Request) {
	if s.ApplyListen == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "listen control is unavailable")
		return
	}
	var body struct {
		Listen *listen.Config `json:"listen"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	if body.Listen == nil {
		writeError(w, http.StatusBadRequest, "bad_request", "listen is required")
		return
	}
	current := s.currentListen()
	next := *body.Listen
	if next.Port == 0 {
		next.Port = current.Port
	}
	if len(next.Hosts) == 0 {
		next.Hosts = current.Hosts
	}
	normalized, err := listen.Normalize(next)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	if err := s.ApplyListen(normalized); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	applied := s.currentListen()
	if err := s.DB.Queries.UpdateServerListen(r.Context(), sqlcdb.UpdateServerListenParams{
		ListenEnabled: boolInt(applied.Enabled),
		ListenPort:    int64(applied.Port),
		ListenHosts:   applied.HostsJSON(),
	}); err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, s.serverPayload())
}

func (s *Server) currentListen() listen.Config {
	if s.Listen != nil {
		return s.Listen.Snapshot()
	}
	return listen.Default()
}

func (s *Server) serverPayload() map[string]any {
	cfg := s.currentListen()
	return map[string]any{
		"fingerprint":        s.PKI.Abbrev,
		"fingerprint_sha256": s.PKI.Full,
		"secret_backend":     s.secretBackend(),
		"pki_ready":          true,
		"listen":             cfg,
		"server_ca_pem":      string(s.PKI.Material.ServerCACert),
	}
}

func (s *Server) expireRegistration(r *http.Request, id string) (sqlcdb.Registration, error) {
	row, err := s.DB.Queries.GetRegistration(r.Context(), id)
	if errors.Is(err, sql.ErrNoRows) {
		return sqlcdb.Registration{}, library.ErrNotFound
	}
	if err != nil {
		return sqlcdb.Registration{}, err
	}
	return s.expireRegistrationRow(r, row)
}

func (s *Server) expireRegistrationRow(r *http.Request, row sqlcdb.Registration) (sqlcdb.Registration, error) {
	if row.Status != "pending" {
		return row, nil
	}
	expires, err := time.Parse(time.RFC3339Nano, row.ExpiresAt)
	if err != nil {
		expires, err = time.Parse(time.RFC3339, row.ExpiresAt)
	}
	if err == nil && time.Now().UTC().After(expires) {
		return s.DB.Queries.UpdateRegistration(r.Context(), sqlcdb.UpdateRegistrationParams{
			Status:   "expired",
			CertPem:  row.CertPem,
			ClientID: row.ClientID,
			ID:       row.ID,
		})
	}
	return row, nil
}

func registrationFromRow(row sqlcdb.Registration) registrationResponse {
	return registrationResponse{
		ID:       row.ID,
		Name:     row.Name,
		Status:   row.Status,
		ClientID: row.ClientID.String,
		CertPEM:  row.CertPem,
	}
}

func validateFriendlyName(name string) error {
	if name == "" || !utf8.ValidString(name) || utf8.RuneCountInString(name) > friendlyNameLimit {
		return errors.New("friendly name is required")
	}
	for _, r := range name {
		if unicode.IsControl(r) {
			return errors.New("friendly name is invalid")
		}
	}
	return nil
}
