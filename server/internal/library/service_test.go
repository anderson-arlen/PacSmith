package library

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
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

func TestPatchReleaseConfigurationPreservesLargeInspectionEvidence(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	svc := &Service{DB: db}
	now := nowUTC()
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "project-large", DisplayName: "Large Vendor App", ArchPackageName: "large-vendor-bin",
		SourceIdentity: "local:large", HistoryJson: "[]", CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	largePreview := strings.Repeat("inspection-evidence-", 70_000)
	body, err := json.Marshal(map[string]any{
		"payload":      []map[string]any{{"path": "opt/vendor/resources.bin", "textPreview": largePreview}},
		"dependencies": []map[string]any{{"rawExpression": "vendor-runtime", "status": "unresolved"}},
		"payloadRules": []any{},
		"state":        "needs-review",
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(body) <= 1<<20 {
		t.Fatalf("test release body is only %d bytes", len(body))
	}
	release, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID: "release-large", ProjectID: project.ID, State: "needs-review", SourceType: "deb",
		VendorVersion: "1.0", OriginalFilename: "large.deb", SourceSha256: strings.Repeat("a", 64),
		ArchPackageName: project.ArchPackageName, ArchPkgrel: 1, BodyJson: string(body),
		CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	dependencies := []any{map[string]any{
		"rawExpression": "vendor-runtime", "status": "resolved", "archPackage": "vulkan-driver",
	}}
	updated, err := svc.PatchReleaseConfiguration(ctx, release.ID, release.Revision,
		map[string]any{"dependencies": dependencies})
	if err != nil {
		t.Fatal(err)
	}
	payload, ok := updated.Document["payload"].([]any)
	if !ok || len(payload) != 1 {
		t.Fatalf("large payload evidence was not preserved: %#v", updated.Document["payload"])
	}
	entry, _ := payload[0].(map[string]any)
	if entry["textPreview"] != largePreview {
		t.Fatal("large payload evidence changed during configuration patch")
	}
	unchanged, err := svc.PatchReleaseConfiguration(ctx, release.ID, updated.Revision,
		map[string]any{"dependencies": dependencies})
	if err != nil {
		t.Fatal(err)
	}
	if unchanged.Revision != updated.Revision {
		t.Fatalf("no-op patch advanced revision from %d to %d", updated.Revision, unchanged.Revision)
	}
	if _, err := svc.PatchReleaseConfiguration(ctx, release.ID, updated.Revision,
		map[string]any{"payload": []any{}}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("inspection evidence mutation error = %v, want ErrInvalid", err)
	}
}

func TestCreateDiscoveredReleasePersistsRemoteCandidate(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	svc := &Service{DB: db}
	now := nowUTC()
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "parsec", DisplayName: "Parsec", ArchPackageName: "parsec-bin",
		SourceIdentity: "direct-url:parsec", HistoryJson: "[]",
		CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	document := map[string]any{
		"state":                  "discovered",
		"originalSourceFilename": "parsec-linux.deb",
		"sourceSha256":           strings.Repeat("c", 64),
		"sourceUrl":              "https://builds.parsec.app/package/parsec-linux.deb",
		"debian":                 map[string]any{"version": "150-105"},
		"update": map[string]any{
			"strategy": "Direct URL", "directUrlEtag": "\"opaque\"",
		},
	}
	created, err := svc.CreateDiscoveredRelease(ctx, project.ID, document)
	if err != nil {
		t.Fatal(err)
	}
	if created.State != "discovered" || created.VendorVersion != "150-105" ||
		created.SourceSHA256 != strings.Repeat("c", 64) {
		t.Fatalf("created release %+v", created)
	}
	update, ok := mapValue(created.Document, "update")
	if !ok || stringValue(update, "directUrlEtag") != "\"opaque\"" {
		t.Fatalf("update state %#v", created.Document["update"])
	}
	again, err := svc.CreateDiscoveredRelease(ctx, project.ID, document)
	if err != nil {
		t.Fatal(err)
	}
	if again.ID != created.ID {
		t.Fatalf("duplicate candidate ID %q, want %q", again.ID, created.ID)
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

func TestGetReleaseDerivesBuildSummaryAndArtifacts(t *testing.T) {
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
	svc := &Service{DB: db, Artifacts: &artifact.Registry{DB: db, Store: store}}
	now := nowUTC()
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "project-build", DisplayName: "Built App", ArchPackageName: "built-app-bin",
		SourceIdentity: "local:built", HistoryJson: "[]", CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	body, err := json.Marshal(map[string]any{
		"buildStatus": "never-built", "state": "needs-review", "lastBuildLog": "",
	})
	if err != nil {
		t.Fatal(err)
	}
	release, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID: "release-build", ProjectID: project.ID, State: "needs-review", SourceType: "deb",
		VendorVersion: "2.0", OriginalFilename: "built.deb", SourceSha256: strings.Repeat("c", 64),
		ArchPackageName: project.ArchPackageName, ArchPkgrel: 1, BodyJson: string(body),
		CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	built, err := svc.Artifacts.Put(ctx, "built-app-bin-2.0-3-x86_64.pkg.tar.zst",
		"arch_package", bytes.NewReader([]byte("package")))
	if err != nil {
		t.Fatal(err)
	}
	build, err := db.Queries.InsertBuild(ctx, sqlcdb.InsertBuildParams{
		ID: "build-success", ReleaseID: release.ID, Status: "succeeded", LogText: "done",
		StartedAt: nullString(now), FinishedAt: nullString(now),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
		ReleaseID: release.ID, ArtifactID: built.ID, Role: "built_package",
	}); err != nil {
		t.Fatal(err)
	}
	if err := db.Queries.InsertBuildArtifact(ctx, sqlcdb.InsertBuildArtifactParams{
		BuildID: build.ID, ArtifactID: built.ID,
	}); err != nil {
		t.Fatal(err)
	}

	got, err := svc.GetRelease(ctx, release.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != "built" || got.Document["buildStatus"] != "succeeded" {
		t.Fatalf("derived release state = %q, status = %#v", got.State, got.Document["buildStatus"])
	}
	builds, ok := got.Document["builds"].([]map[string]any)
	if !ok || len(builds) != 1 {
		t.Fatalf("build records = %#v", got.Document["builds"])
	}
	artifacts, ok := builds[0]["artifacts"].([]map[string]any)
	if !ok || len(artifacts) != 1 {
		t.Fatalf("build artifacts = %#v", builds[0]["artifacts"])
	}
	if artifacts[0]["packageName"] != "built-app-bin" ||
		artifacts[0]["packageVersion"] != "2.0-3" {
		t.Fatalf("artifact metadata = %#v", artifacts[0])
	}
}

func TestPrepareBuildIdentityIncrementsPkgrel(t *testing.T) {
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
	now := nowUTC()
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID:              "proj-rel",
		DisplayName:     "Rel",
		ArchPackageName: "rel-bin",
		SourceIdentity:  "local:rel",
		HistoryJson:     "[]",
		CreatedAt:       now,
		ModifiedAt:      now,
	})
	if err != nil {
		t.Fatal(err)
	}
	pkg, err := svc.Artifacts.Put(ctx, "rel-bin-1.5.0-1-x86_64.pkg.tar.zst", "arch_package", bytes.NewReader([]byte("pkg")))
	if err != nil {
		t.Fatal(err)
	}
	release, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID:               "rel-1",
		ProjectID:        project.ID,
		State:            "ready",
		SourceType:       "deb",
		VendorVersion:    "1.5.0",
		OriginalFilename: "rel.deb",
		SourceSha256:     strings.Repeat("b", 64),
		ArchPackageName:  "rel-bin",
		ArchPkgrel:       1,
		BodyJson:         `{"debian":{"package":"rel","version":"1.5.0"},"pkgbuildManuallyModified":true}`,
		CreatedAt:        now,
		ModifiedAt:       now,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
		ReleaseID:  release.ID,
		ArtifactID: pkg.ID,
		Role:       "built_package",
	}); err != nil {
		t.Fatal(err)
	}
	updated, vars, _, err := svc.prepareBuildIdentity(ctx, release, "pkgbuild", "vars")
	if err != nil {
		t.Fatal(err)
	}
	if updated.ArchPkgrel != 2 {
		t.Fatalf("pkgrel %d, want 2", updated.ArchPkgrel)
	}
	if !strings.Contains(vars, "_PACSMITH_PKGREL='2'") {
		t.Fatalf("vars did not record pkgrel 2:\n%s", vars)
	}
}

func TestCleanupRespectsRepoRoots(t *testing.T) {
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
	fileStore, err := secret.NewFileStore(filepath.Join(root, "secrets"))
	if err != nil {
		t.Fatal(err)
	}
	registry := &artifact.Registry{DB: db, Store: store}
	repoSvc := repo.New(db, registry, secret.NewLockedStore(secret.BackendFile, fileStore),
		filepath.Join(root, "work"), filepath.Join(root, "gnupg"))
	svc := &Service{DB: db, Artifacts: registry, WorkDir: filepath.Join(root, "releases"), Repo: repoSvc}
	kept, err := registry.Put(ctx, "kept.bin", "unknown", bytes.NewReader([]byte("kept-root")))
	if err != nil {
		t.Fatal(err)
	}
	orphan, err := registry.Put(ctx, "orphan.bin", "unknown", bytes.NewReader([]byte("orphan")))
	if err != nil {
		t.Fatal(err)
	}
	now := nowUTC()
	if err := db.Queries.UpsertChannelEntry(ctx, sqlcdb.UpsertChannelEntryParams{
		Channel:     "unstable",
		Arch:        "x86_64",
		Pkgname:     "demo-bin",
		Epoch:       0,
		Pkgver:      "1.0.0",
		Pkgrel:      "1",
		ArtifactID:  kept.ID,
		Filename:    "demo-bin-1.0.0-1-x86_64.pkg.tar.zst",
		PublishedAt: now,
	}); err != nil {
		t.Fatal(err)
	}
	if err := svc.Cleanup(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := registry.Get(ctx, kept.ID); err != nil {
		t.Fatal("repo-referenced artifact was deleted")
	}
	if _, err := registry.Get(ctx, orphan.ID); err == nil {
		t.Fatal("unreferenced artifact survived cleanup")
	}
}
