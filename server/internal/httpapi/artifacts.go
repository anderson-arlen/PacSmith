package httpapi

import (
	"errors"
	"io"
	"net/http"
	"strconv"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
)

func (s *Server) createArtifact(w http.ResponseWriter, r *http.Request) {
	filename := r.Header.Get("Pacsmith-Filename")
	if filename == "" {
		filename = r.URL.Query().Get("filename")
	}
	kind := r.Header.Get("Pacsmith-Kind")
	if kind == "" {
		kind = r.URL.Query().Get("kind")
	}
	body := http.MaxBytesReader(w, r.Body, artifact.MaxBytes)
	record, err := s.Artifacts.Put(r.Context(), filename, kind, body)
	if err != nil {
		var maxBytesErr *http.MaxBytesError
		if errors.As(err, &maxBytesErr) {
			writeError(w, http.StatusRequestEntityTooLarge, "too_large", "artifact exceeds the maximum size")
			return
		}
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, record)
}

func (s *Server) getArtifact(w http.ResponseWriter, r *http.Request) {
	record, err := s.Artifacts.Get(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, record)
}

func (s *Server) getArtifactContent(w http.ResponseWriter, r *http.Request) {
	record, file, err := s.Artifacts.Open(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	defer file.Close()
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(record.SizeBytes, 10))
	w.Header().Set("Content-Disposition", `attachment; filename="`+sanitizeDisposition(record.OriginalFilename)+`"`)
	w.Header().Set("Pacsmith-Sha256", record.SHA256)
	w.Header().Set("Pacsmith-Kind", record.Kind)
	w.WriteHeader(http.StatusOK)
	_, _ = io.Copy(w, file)
}

func sanitizeDisposition(name string) string {
	cleaned, err := artifact.SanitizeFilename(name)
	if err != nil {
		return "artifact"
	}
	return cleaned
}
