package library

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"path/filepath"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func identityService(t *testing.T) (*Service, []sqlcdb.Release) {
	t.Helper()
	ctx := context.Background()
	dir := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(dir, "library.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	store, err := artifact.New(filepath.Join(dir, "objects"), filepath.Join(dir, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	s := &Service{DB: db, Artifacts: &artifact.Registry{DB: db, Store: store}, WorkDir: filepath.Join(dir, "work")}
	_, err = db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "word-squadron", DisplayName: "Word Squadron", ArchPackageName: "word_squadron-bin", HistoryJson: "[]",
	})
	if err != nil {
		t.Fatal(err)
	}
	var rows []sqlcdb.Release
	for index, id := range []string{"older", "newer"} {
		source, err := s.Artifacts.Put(ctx, "Word_Squadron-0.1.0+1-x86_64.AppImage", "vendor", bytes.NewBufferString(id))
		if err != nil {
			t.Fatal(err)
		}
		row, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
			ID: id, ProjectID: "word-squadron", State: "ready", SourceType: "appimage", VendorVersion: "0.1.0+1",
			OriginalFilename: source.OriginalFilename, SourceSha256: source.SHA256, SourceArtifactID: nullString(source.ID),
			ArchPackageName: "word_squadron-bin", ArchPkgrel: int64(3 - index),
			BodyJson:  `{"debian":{"version":"0.1.0+1"},"buildStatus":"never-built","pkgbuildManuallyModified":true,"customPkgbuild":"source ./pacsmith.vars\npackage() { :; }\n"}`,
			CreatedAt: []string{"2026-10-17T17:08:43.948Z", "2026-10-17T17:17:40.799Z"}[index],
		})
		if err != nil {
			t.Fatal(err)
		}
		rows = append(rows, row)
	}
	return s, rows
}

func TestEqualVersionBuildReservesRevisionAboveSibling(t *testing.T) {
	s, rows := identityService(t)
	ctx := context.Background()
	updated, vars, _, err := s.prepareBuildIdentity(ctx, rows[1], "custom", "vars")
	if err != nil {
		t.Fatal(err)
	}
	if updated.ID != "newer" || updated.ArchPkgrel != 4 ||
		!strings.Contains(vars, "_PACSMITH_RELEASE='newer'") || !strings.Contains(vars, "_PACSMITH_PKGREL='4'") ||
		!strings.Contains(vars, rows[1].SourceSha256) {
		t.Fatalf("wrong build identity: %+v\n%s", updated, vars)
	}
	older, err := s.DB.Queries.GetRelease(ctx, "older")
	if err != nil {
		t.Fatal(err)
	}
	if older.ArchPkgrel != 3 {
		t.Fatal("sibling revision changed")
	}
}

func TestClientCannotInventBuildingState(t *testing.T) {
	s, rows := identityService(t)
	_, err := s.PatchReleaseConfiguration(context.Background(), rows[1].ID, rows[1].Revision,
		map[string]any{"buildStatus": "building"})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("runtime state accepted: %v", err)
	}
}

func TestBuildFailureFinalizesOnlyRequestedArtifact(t *testing.T) {
	for _, canceled := range []bool{false, true} {
		name := "setup-error"
		if canceled {
			name = "canceled"
		}
		t.Run(name, func(t *testing.T) {
			s, _ := identityService(t)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			if canceled {
				t.Setenv("PACSMITH_PODMAN", "/usr/bin/true")
			} else {
				t.Setenv("PACSMITH_PODMAN", "/missing-podman")
			}
			_, err := s.BuildRelease(ctx, "newer", func(string) {
				if canceled {
					cancel()
				}
			}, false)
			if err == nil {
				t.Fatal("expected build failure")
			}
			for _, id := range []string{"older", "newer"} {
				row, err := s.DB.Queries.GetRelease(context.Background(), id)
				if err != nil {
					t.Fatal(err)
				}
				var body map[string]any
				if err := json.Unmarshal([]byte(row.BodyJson), &body); err != nil {
					t.Fatal(err)
				}
				builds, err := s.DB.Queries.ListBuildsForRelease(context.Background(), id)
				if err != nil {
					t.Fatal(err)
				}
				if id == "older" {
					if len(builds) != 0 || body["buildStatus"] != "never-built" {
						t.Fatalf("sibling changed: %+v", row)
					}
				} else if len(builds) != 1 || builds[0].Status != "failed" || builds[0].LogText == "" || body["buildStatus"] != "failed" {
					t.Fatalf("requested build left unfinished: %+v %+v", body, builds)
				}
			}
		})
	}
}
