package artifact

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

var ErrNotFound = errors.New("artifact not found")

const (
	MaxBytes         = 4 << 30
	MaxFilenameBytes = 255
)

var kindPattern = regexp.MustCompile(`^[a-z][a-z0-9_]{0,63}$`)

type Registry struct {
	DB    *sqlite.DB
	Store *Store
}

type Record struct {
	ID               string `json:"id"`
	SHA256           string `json:"sha256"`
	SizeBytes        int64  `json:"size_bytes"`
	OriginalFilename string `json:"original_filename"`
	Kind             string `json:"kind"`
	CreatedAt        string `json:"created_at"`
}

func (r *Registry) Put(ctx context.Context, filename, kind string, body io.Reader) (Record, error) {
	filename, err := SanitizeFilename(filename)
	if err != nil {
		return Record{}, err
	}
	kind, err = SanitizeKind(kind)
	if err != nil {
		return Record{}, err
	}
	object, err := r.Store.Ingest(body)
	if err != nil {
		return Record{}, err
	}
	existing, err := r.DB.Queries.GetArtifactBySHA256(ctx, object.SHA256)
	if err == nil {
		return recordFrom(existing), nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return Record{}, err
	}
	row, err := r.DB.Queries.InsertArtifact(ctx, sqlcdb.InsertArtifactParams{
		ID:               uuid.NewString(),
		Sha256:           object.SHA256,
		SizeBytes:        object.Size,
		OriginalFilename: filename,
		Kind:             kind,
		CreatedAt:        time.Now().UTC().Format(time.RFC3339Nano),
	})
	if err != nil {
		if isUniqueConstraint(err) {
			existing, getErr := r.DB.Queries.GetArtifactBySHA256(ctx, object.SHA256)
			if getErr != nil {
				return Record{}, err
			}
			return recordFrom(existing), nil
		}
		return Record{}, fmt.Errorf("record artifact: %w", err)
	}
	return recordFrom(row), nil
}

func (r *Registry) Get(ctx context.Context, id string) (Record, error) {
	if _, err := uuid.Parse(id); err != nil {
		return Record{}, fmt.Errorf("invalid artifact id")
	}
	row, err := r.DB.Queries.GetArtifact(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return Record{}, ErrNotFound
	}
	if err != nil {
		return Record{}, err
	}
	return recordFrom(row), nil
}

func (r *Registry) Delete(ctx context.Context, id string) error {
	record, err := r.Get(ctx, id)
	if err != nil {
		return err
	}
	if err := r.DB.Queries.DeleteArtifact(ctx, id); err != nil {
		return err
	}
	return r.Store.Remove(record.SHA256)
}

func (r *Registry) Open(ctx context.Context, id string) (Record, *os.File, error) {
	record, err := r.Get(ctx, id)
	if err != nil {
		return Record{}, nil, err
	}
	file, size, err := r.Store.Open(record.SHA256)
	if err != nil {
		return Record{}, nil, err
	}
	if size != record.SizeBytes {
		_ = file.Close()
		return Record{}, nil, fmt.Errorf("artifact object size does not match database")
	}
	return record, file, nil
}

func recordFrom(row sqlcdb.Artifact) Record {
	return Record{
		ID:               row.ID,
		SHA256:           row.Sha256,
		SizeBytes:        row.SizeBytes,
		OriginalFilename: row.OriginalFilename,
		Kind:             row.Kind,
		CreatedAt:        row.CreatedAt,
	}
}

func SanitizeFilename(name string) (string, error) {
	name = strings.TrimSpace(name)
	name = filepath.Base(filepath.ToSlash(name))
	if name == "" || name == "." || name == ".." {
		return "", fmt.Errorf("artifact filename is required")
	}
	if strings.ContainsRune(name, 0) || !utf8.ValidString(name) {
		return "", fmt.Errorf("artifact filename is invalid")
	}
	if len(name) > MaxFilenameBytes {
		return "", fmt.Errorf("artifact filename exceeds %d bytes", MaxFilenameBytes)
	}
	if strings.ContainsAny(name, `/\`) {
		return "", fmt.Errorf("artifact filename must not contain a path")
	}
	return name, nil
}

func SanitizeKind(kind string) (string, error) {
	kind = strings.TrimSpace(strings.ToLower(kind))
	if kind == "" {
		kind = "unknown"
	}
	if !kindPattern.MatchString(kind) {
		return "", fmt.Errorf("artifact kind is invalid")
	}
	return kind, nil
}

func isUniqueConstraint(err error) bool {
	if err == nil {
		return false
	}
	message := strings.ToLower(err.Error())
	return strings.Contains(message, "unique constraint") || strings.Contains(message, "constraint failed")
}
