package library

import (
	"context"
	"database/sql"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

func storageFixture(t *testing.T) (*Service, context.Context) {
	t.Helper()
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "library.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	store, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	s := &Service{DB: db, Artifacts: &artifact.Registry{DB: db, Store: store}, WorkDir: filepath.Join(root, "work")}
	for _, id := range []string{"project", "other"} {
		_, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
			ID: id, DisplayName: id, SourceIdentity: id, HistoryJson: "[]", CreatedAt: nowUTC(), ModifiedAt: nowUTC(),
		})
		if err != nil {
			t.Fatal(err)
		}
	}
	return s, ctx
}

func storageArtifact(t *testing.T, s *Service, age time.Duration) sqlcdb.Artifact {
	t.Helper()
	object, err := s.Artifacts.Store.Ingest(strings.NewReader(uuid.NewString()))
	if err != nil {
		t.Fatal(err)
	}
	row, err := s.DB.Queries.InsertArtifact(context.Background(), sqlcdb.InsertArtifactParams{
		ID: uuid.NewString(), Sha256: object.SHA256, SizeBytes: object.Size,
		OriginalFilename: "package.bin", Kind: "source", CreatedAt: time.Now().Add(-age).UTC().Format(time.RFC3339Nano),
	})
	if err != nil {
		t.Fatal(err)
	}
	return row
}

func storageRelease(t *testing.T, s *Service, project, id string, source sqlcdb.Artifact) {
	t.Helper()
	_, err := s.DB.Queries.InsertRelease(context.Background(), sqlcdb.InsertReleaseParams{
		ID: id, ProjectID: project, State: "ready", SourceType: "elf", ArchPkgrel: 1,
		SourceArtifactID: nullString(source.ID), SourceSha256: source.Sha256, BodyJson: "{}",
		CreatedAt: nowUTC(), ModifiedAt: nowUTC(),
	})
	if err != nil {
		t.Fatal(err)
	}
}

func storageFile(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("build data"), 0600); err != nil {
		t.Fatal(err)
	}
}

func assertStorageArtifact(t *testing.T, s *Service, a sqlcdb.Artifact, exists bool) {
	t.Helper()
	_, err := s.DB.Queries.GetArtifact(context.Background(), a.ID)
	if exists && err != nil || !exists && !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("artifact row exists=%v: %v", exists, err)
	}
	onDisk, err := s.Artifacts.Store.Exists(a.Sha256)
	if err != nil || onDisk != exists {
		t.Fatalf("artifact file exists=%v, want %v: %v", onDisk, exists, err)
	}
}

func TestDeleteReleaseReclaimsSourcesBuildHistoryAndWorkspaces(t *testing.T) {
	s, ctx := storageFixture(t)
	source, built, historical := storageArtifact(t, s, 0), storageArtifact(t, s, 0), storageArtifact(t, s, 0)
	storageRelease(t, s, "project", "release", source)
	if err := s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
		ReleaseID: "release", ArtifactID: built.ID, Role: "built_package",
	}); err != nil {
		t.Fatal(err)
	}
	_, err := s.DB.Queries.InsertBuild(ctx, sqlcdb.InsertBuildParams{ID: "build", ReleaseID: "release", Status: "succeeded"})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.DB.Queries.InsertBuildArtifact(ctx, sqlcdb.InsertBuildArtifactParams{BuildID: "build", ArtifactID: historical.ID}); err != nil {
		t.Fatal(err)
	}
	if err := s.DB.Queries.UpsertSoak(ctx, sqlcdb.UpsertSoakParams{
		Pkgname: "old-package", Arch: "x86_64", Pkgver: "1", Pkgrel: "1", Status: "promoted",
		ProjectID: nullString("project"), ReleaseID: nullString("release"), ArtifactID: historical.ID,
		SoakStartedAt: nowUTC(), EligibleAt: nowUTC(),
	}); err != nil {
		t.Fatal(err)
	}
	for _, dir := range []string{"release", "release.preserved-src"} {
		storageFile(t, filepath.Join(s.WorkDir, "releases", dir, "source"))
	}
	if err := s.DeleteRelease(ctx, "release"); err != nil {
		t.Fatal(err)
	}
	for _, a := range []sqlcdb.Artifact{source, built, historical} {
		assertStorageArtifact(t, s, a, false)
	}
	entries, err := os.ReadDir(filepath.Join(s.WorkDir, "releases"))
	if err != nil || len(entries) != 0 {
		t.Fatalf("deleted release workspaces remain: %v %v", entries, err)
	}
}

func TestDeleteReleasePreservesSharedAndPublishedArtifacts(t *testing.T) {
	s, ctx := storageFixture(t)
	shared, published := storageArtifact(t, s, 0), storageArtifact(t, s, 0)
	storageRelease(t, s, "project", "deleted", shared)
	storageRelease(t, s, "other", "kept", shared)
	if err := s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{ReleaseID: "deleted", ArtifactID: published.ID, Role: "built_package"}); err != nil {
		t.Fatal(err)
	}
	if err := s.DB.Queries.UpsertChannelEntry(ctx, sqlcdb.UpsertChannelEntryParams{
		Channel: "unstable", Arch: "x86_64", Pkgname: "example", Pkgver: "1", Pkgrel: "1", ArtifactID: published.ID,
		ProjectID: nullString("project"), ReleaseID: nullString("deleted"), Filename: "example.pkg.tar.zst", PublishedAt: nowUTC(),
	}); err != nil {
		t.Fatal(err)
	}
	if err := s.DeleteRelease(ctx, "deleted"); err != nil {
		t.Fatal(err)
	}
	assertStorageArtifact(t, s, shared, true)
	assertStorageArtifact(t, s, published, true)
	if err := s.DeleteProject(ctx, "project"); err != nil {
		t.Fatal(err)
	}
	assertStorageArtifact(t, s, shared, true)
	if err := s.DeleteProject(ctx, "other"); err != nil {
		t.Fatal(err)
	}
	assertStorageArtifact(t, s, shared, false)
}

func TestStorageCollectionRepairsOldDeletionsAndPreservesRecentUploads(t *testing.T) {
	s, ctx := storageFixture(t)
	old, recent, kept := storageArtifact(t, s, 48*time.Hour), storageArtifact(t, s, 0), storageArtifact(t, s, 48*time.Hour)
	storageRelease(t, s, "project", "kept", kept)
	summaries, err := s.ListProjectSummaries(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, project := range summaries {
		if project.ID == "project" && project.Releases[0].Document["sourceArtifactId"] != kept.ID {
			t.Fatal("library summary omits the source needed to preserve its client cache")
		}
	}
	for _, dir := range []string{"releases/deleted", "releases/kept", "cache/sources/deleted", "cache/ccache/deleted", "cache/sources/project", "cache/pacman"} {
		storageFile(t, filepath.Join(s.WorkDir, dir, "data"))
	}
	if err := s.CollectStorage(ctx); err != nil {
		t.Fatal(err)
	}
	assertStorageArtifact(t, s, old, false)
	assertStorageArtifact(t, s, recent, true)
	assertStorageArtifact(t, s, kept, true)
	for _, dir := range []string{"releases/deleted", "cache/sources/deleted", "cache/ccache/deleted"} {
		if _, err := os.Stat(filepath.Join(s.WorkDir, dir)); !os.IsNotExist(err) {
			t.Fatalf("orphan survived: %s: %v", dir, err)
		}
	}
	for _, dir := range []string{"releases/kept", "cache/sources/project", "cache/pacman"} {
		if _, err := os.Stat(filepath.Join(s.WorkDir, dir)); err != nil {
			t.Fatalf("live cache removed: %s: %v", dir, err)
		}
	}
	if err := s.DeleteProject(ctx, "project"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(s.WorkDir, "cache/sources/project")); !os.IsNotExist(err) {
		t.Fatal("project cache survived deletion")
	}
}

func TestDeletionRejectsActiveJobsAndKeepsTheirStorage(t *testing.T) {
	s, ctx := storageFixture(t)
	source := storageArtifact(t, s, 48*time.Hour)
	storageRelease(t, s, "project", "release", source)
	_, err := s.DB.Queries.InsertJob(ctx, sqlcdb.InsertJobParams{ID: "job", Kind: "build", Status: "queued", ReleaseID: nullString("release"), ProjectID: nullString("project"), PayloadJson: "{}", CreatedAt: nowUTC()})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.DeleteRelease(ctx, "release"); !errors.Is(err, ErrConflict) {
		t.Fatalf("delete busy release: %v", err)
	}
	if err := s.DeleteProject(ctx, "project"); !errors.Is(err, ErrConflict) {
		t.Fatalf("delete busy project: %v", err)
	}
	assertStorageArtifact(t, s, source, true)
}

func TestStorageRemovalFailureIsReportedAndRetried(t *testing.T) {
	s, ctx := storageFixture(t)
	a := storageArtifact(t, s, 48*time.Hour)
	path, _ := s.Artifacts.Store.Path(a.Sha256)
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	storageFile(t, filepath.Join(path, "blocked"))
	if err := s.CollectStorage(ctx); err == nil {
		t.Fatal("file removal failure was ignored")
	}
	if _, err := s.DB.Queries.GetArtifact(ctx, a.ID); err != nil {
		t.Fatal("failed cleanup lost its retry record", err)
	}
	if err := os.RemoveAll(path); err != nil {
		t.Fatal(err)
	}
	storageFile(t, path)
	if err := s.CollectStorage(ctx); err != nil {
		t.Fatal(err)
	}
	assertStorageArtifact(t, s, a, false)
}
