package library

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestWriteReleaseIconCopiesArtifact(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	store, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	svc := &Service{
		DB:        db,
		Artifacts: &artifact.Registry{DB: db, Store: store},
		WorkDir:   filepath.Join(root, "work"),
	}

	png := []byte{
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
		0, 0, 0, 13, 'I', 'H', 'D', 'R',
		0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0, 0x90, 0x77, 0x53, 0xde,
		0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82,
	}
	icon, err := svc.Artifacts.Put(ctx, "vscode.png", "icon", bytes.NewReader(png))
	if err != nil {
		t.Fatal(err)
	}

	now := nowUTC()
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID:              "proj-code",
		DisplayName:     "Visual Studio Code",
		ArchPackageName: "code-bin",
		SourceIdentity:  "local:code",
		HistoryJson:     "[]",
		CreatedAt:       now,
		ModifiedAt:      now,
	})
	if err != nil {
		t.Fatal(err)
	}
	body, err := json.Marshal(map[string]any{
		"originalSourceFilename": "code.deb",
		"installMapping": map[string]any{
			"icon": map[string]any{
				"sourceKind": "payload",
				"sourcePath": "usr/share/pixmaps/vscode.png",
				"sha256":     icon.SHA256,
				"format":     "png",
				"iconName":   "code",
			},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	release, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID:               "rel-code",
		ProjectID:        project.ID,
		State:            "needs-review",
		SourceType:       "deb",
		VendorVersion:    "1.133.0",
		OriginalFilename: "code.deb",
		SourceSha256:     strings.Repeat("a", 64),
		ArchPackageName:  "code-bin",
		ArchPkgrel:       1,
		BodyJson:         string(body),
		CreatedAt:        now,
		ModifiedAt:       now,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
		ReleaseID:  release.ID,
		ArtifactID: icon.ID,
		Role:       "icon",
	}); err != nil {
		t.Fatal(err)
	}

	work := filepath.Join(root, "build")
	if err := os.MkdirAll(work, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := svc.writeReleaseIcon(ctx, release, work); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(work, "pacsmith-icon.png"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, png) {
		t.Fatalf("wrote %d bytes, want %d", len(got), len(png))
	}
}

func TestUnconfiguredIconArtifactIsNotExposed(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	store, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	svc := &Service{
		DB:        db,
		Artifacts: &artifact.Registry{DB: db, Store: store},
		WorkDir:   filepath.Join(root, "work"),
	}

	png := []byte{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}
	icon, err := svc.Artifacts.Put(ctx, "stale.png", "icon", bytes.NewReader(png))
	if err != nil {
		t.Fatal(err)
	}

	now := nowUTC()
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID:              "proj-brave",
		DisplayName:     "Brave",
		ArchPackageName: "brave-bin",
		SourceIdentity:  "local:brave",
		HistoryJson:     "[]",
		CreatedAt:       now,
		ModifiedAt:      now,
	})
	if err != nil {
		t.Fatal(err)
	}
	body, err := json.Marshal(map[string]any{
		"installMapping": map[string]any{
			"icon": map[string]any{"sourceKind": "none"},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	release, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID:               "rel-brave",
		ProjectID:        project.ID,
		State:            "needs-review",
		SourceType:       "deb",
		VendorVersion:    "1.93.137",
		OriginalFilename: "brave.deb",
		SourceSha256:     strings.Repeat("b", 64),
		ArchPackageName:  "brave-bin",
		ArchPkgrel:       1,
		BodyJson:         string(body),
		CreatedAt:        now,
		ModifiedAt:       now,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
		ReleaseID:  release.ID,
		ArtifactID: icon.ID,
		Role:       "icon",
	}); err != nil {
		t.Fatal(err)
	}

	got, err := svc.GetRelease(ctx, release.ID)
	if err != nil {
		t.Fatal(err)
	}
	if id, _ := got.Document["iconArtifactId"].(string); id != "" {
		t.Fatalf("unconfigured icon artifact leaked as %q", id)
	}

	svc.replaceInspectedIcon(ctx, release.ID, inspect.Analysis{})
	artifacts, err := db.Queries.ListReleaseArtifacts(ctx, release.ID)
	if err != nil {
		t.Fatal(err)
	}
	for _, item := range artifacts {
		if item.Role == "icon" {
			t.Fatal("reanalyze left a stale icon artifact attached")
		}
	}
}
