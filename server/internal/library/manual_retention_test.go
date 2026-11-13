package library

import (
	"database/sql"
	"errors"
	"fmt"
	"path/filepath"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestCleanupPrunesManualPromotionHistory(t *testing.T) {
	for _, stable := range []bool{false, true} {
		t.Run(fmt.Sprintf("stable=%v", stable), func(t *testing.T) {
			s, ctx := storageFixture(t)
			s.Repo = repo.New(s.DB, s.Artifacts, nil, filepath.Join(t.TempDir(), "repo"), "")
			settings, err := s.DB.Queries.GetLibrarySettings(ctx)
			if err != nil {
				t.Fatal(err)
			}
			update := librarySettingsUpdate(settings)
			update.RetentionVersions = 2
			if _, err := s.DB.Queries.UpdateLibrarySettings(ctx, update); err != nil {
				t.Fatal(err)
			}
			repoSettings, err := s.DB.Queries.GetRepoSettings(ctx)
			if err != nil {
				t.Fatal(err)
			}
			repoUpdate := repoSettingsUpdate(repoSettings)
			repoUpdate.StableEnabled = 1
			if _, err := s.DB.Queries.UpdateRepoSettings(ctx, repoUpdate); err != nil {
				t.Fatal(err)
			}
			if err := s.DB.Queries.UpsertProjectRepoPolicy(ctx, sqlcdb.UpsertProjectRepoPolicyParams{
				ProjectID: "project", AutomaticSoak: 0, SoakSecondsOverride: -1,
			}); err != nil {
				t.Fatal(err)
			}
			var sources, packages []sqlcdb.Artifact
			for i := 0; i < 6; i++ {
				id, version := fmt.Sprintf("release-%d", i), fmt.Sprintf("%d.0", i)
				source, built := storageArtifact(t, s, 0), storageArtifact(t, s, 0)
				sources, packages = append(sources, source), append(packages, built)
				stamp := time.Date(2026, 1, i+1, 0, 0, 0, 0, time.UTC).Format(time.RFC3339)
				if _, err := s.DB.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
					ID: id, ProjectID: "project", State: "built", SourceType: "deb", ArchPkgrel: 1,
					VendorVersion: version, SourceArtifactID: nullString(source.ID), SourceSha256: source.Sha256,
					BodyJson: "{}", CreatedAt: stamp, ModifiedAt: stamp,
				}); err != nil {
					t.Fatal(err)
				}
				if err := s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
					ReleaseID: id, ArtifactID: built.ID, Role: "built_package",
				}); err != nil {
					t.Fatal(err)
				}
				// Reproduce the legacy timers that never finish for manual projects.
				if err := s.DB.Queries.UpsertSoak(ctx, sqlcdb.UpsertSoakParams{
					ProjectID: nullString("project"), ReleaseID: nullString(id), Pkgname: "demo-bin",
					Arch: "x86_64", Pkgver: version, Pkgrel: "1", ArtifactID: built.ID,
					SoakStartedAt: stamp, EligibleAt: stamp, Status: "soaking",
				}); err != nil {
					t.Fatal(err)
				}
				storageFile(t, filepath.Join(s.WorkDir, "releases", id, "source"))
			}
			for _, channel := range []string{"unstable", "stable"} {
				index := 5
				if channel == "stable" {
					if !stable {
						continue
					}
					index = 3
				}
				if err := s.DB.Queries.UpsertChannelEntry(ctx, sqlcdb.UpsertChannelEntryParams{
					Channel: channel, Arch: "x86_64", Pkgname: "demo-bin", ProjectID: nullString("project"),
					ReleaseID: nullString(fmt.Sprintf("release-%d", index)), Pkgver: fmt.Sprintf("%d.0", index),
					Pkgrel: "1", ArtifactID: packages[index].ID, Filename: "demo.pkg.tar.zst", PublishedAt: nowUTC(),
				}); err != nil {
					t.Fatal(err)
				}
			}
			if err := s.Cleanup(ctx); err != nil {
				t.Fatal(err)
			}
			firstKept := 3
			if stable {
				firstKept = 1
			}
			for i := range sources {
				kept := i >= firstKept
				_, err := s.DB.Queries.GetRelease(ctx, fmt.Sprintf("release-%d", i))
				if kept && err != nil || !kept && !errors.Is(err, sql.ErrNoRows) {
					t.Fatalf("release %d kept=%v: %v", i, kept, err)
				}
				assertStorageArtifact(t, s, sources[i], kept)
				assertStorageArtifact(t, s, packages[i], kept)
			}
			soaks, err := s.DB.Queries.ListSoaks(ctx)
			if err != nil || len(soaks) != 0 {
				t.Fatalf("disabled automatic timers remain: %+v, %v", soaks, err)
			}
		})
	}
}
