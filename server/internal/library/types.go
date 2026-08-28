package library

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

var (
	ErrNotFound = errors.New("not found")
	ErrConflict = errors.New("revision conflict")
	ErrInvalid  = errors.New("invalid request")
)

type HistoryEntry struct {
	Timestamp string `json:"timestamp"`
	Event     string `json:"event"`
	Detail    string `json:"detail"`
}

type Project struct {
	ID                   string         `json:"id"`
	Revision             int64          `json:"revision"`
	DisplayName          string         `json:"displayName"`
	ArchPackageName      string         `json:"archPackageName"`
	VendorName           string         `json:"vendorName"`
	SourceIdentity       string         `json:"sourceIdentity"`
	IconSha256           string         `json:"iconSha256"`
	History              []HistoryEntry `json:"history"`
	CreatedAt            string         `json:"createdAt"`
	ModifiedAt           string         `json:"modifiedAt"`
	Releases             []Release      `json:"releases,omitempty"`
	RepoPublish          bool           `json:"-"`
	RepoPkgnameOverride  string         `json:"-"`
	RepoPublishedPkgname string         `json:"-"`
}

type Release struct {
	ID              string          `json:"id"`
	ProjectID       string          `json:"projectId"`
	Revision        int64           `json:"revision"`
	State           string          `json:"state"`
	SourceType      string          `json:"sourceType"`
	VendorVersion   string          `json:"vendorVersion"`
	Body            json.RawMessage `json:"-"`
	ArchPackageName string          `json:"archPackageName"`
	SourceSHA256    string          `json:"sourceSha256"`
	CreatedAt       string          `json:"createdAt"`
	ModifiedAt      string          `json:"modifiedAt"`
	Document        map[string]any  `json:"document"`
}

func nowUTC() string {
	return time.Now().UTC().Truncate(time.Millisecond).Format("2006-01-02T15:04:05.000Z07:00")
}

func nullString(value string) sql.NullString {
	value = strings.TrimSpace(value)
	if value == "" {
		return sql.NullString{}
	}
	return sql.NullString{String: value, Valid: true}
}

func decodeHistory(raw string) []HistoryEntry {
	if strings.TrimSpace(raw) == "" {
		return []HistoryEntry{}
	}
	var history []HistoryEntry
	if err := json.Unmarshal([]byte(raw), &history); err != nil {
		return []HistoryEntry{}
	}
	return history
}

func encodeHistory(history []HistoryEntry) string {
	if history == nil {
		history = []HistoryEntry{}
	}
	raw, err := json.Marshal(history)
	if err != nil {
		return "[]"
	}
	return string(raw)
}

func wrapConflict(err error) error {
	if err == nil {
		return ErrConflict
	}
	return fmt.Errorf("%w: %s", ErrConflict, err.Error())
}
