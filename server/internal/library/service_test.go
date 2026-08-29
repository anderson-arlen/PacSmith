package library

import (
	"archive/tar"
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestBuildParallelismArguments(t *testing.T) {
	arguments := buildParallelismArguments(6)
	if got, want := strings.Join(arguments, " "),
		"MAKEFLAGS=-j6 CMAKE_BUILD_PARALLEL_LEVEL=6"; got != want {
		t.Fatalf("arguments %q, want %q", got, want)
	}
}

func TestPodmanBuildArgumentsConfineCustomBuild(t *testing.T) {
	execution := buildExecution{
		ReleaseID: "release-1", ProjectID: "project-1",
		WorkDir: "/work/releases/release-1", Parallelism: 6,
	}
	arguments := podmanBuildArguments(
		"pacsmith-build-release-1", defaultBuildImage, execution,
		"/work/cache/ccache/project-1", "/work/cache/sources/project-1",
		"/work/cache/pacman")
	joined := strings.Join(arguments, " ")
	for _, required := range []string{
		"--cpus 6", "--pids-limit 4096", "no-new-privileges",
		"src=/work/releases/release-1,dst=/build,rw",
		"src=/work/cache/ccache/project-1,dst=/cache/ccache,rw",
		"src=/work/cache/sources/project-1,dst=/cache/sources,rw",
		"src=/work/cache/pacman,dst=/var/cache/pacman/pkg,rw",
		defaultBuildImage, "makepkg --printsrcinfo", "DownloadUser",
		"pacman -Syu --needed --noconfirm",
		"makepkg --force --noconfirm",
	} {
		if !strings.Contains(joined, required) {
			t.Fatalf("Podman arguments did not contain %q:\n%s", required, joined)
		}
	}
	if strings.Contains(joined, "/home/") || strings.Contains(joined, "podman.sock") {
		t.Fatalf("Podman arguments exposed host authority:\n%s", joined)
	}
}

func TestResetBuildWorkspaceKeepsOnlyCustomSourceTree(t *testing.T) {
	work := filepath.Join(t.TempDir(), "release")
	if err := os.MkdirAll(filepath.Join(work, "src", "project"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(work, "src", "project", "object.o"), []byte("cached"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(work, "stale.patch"), []byte("stale"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := resetBuildWorkspace(work, true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(work, "src", "project", "object.o")); err != nil {
		t.Fatalf("custom source tree was not retained: %v", err)
	}
	if _, err := os.Stat(filepath.Join(work, "stale.patch")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("stale workspace input survived reset: %v", err)
	}
	if info, err := os.Stat(filepath.Join(work, "sources")); err != nil || !info.IsDir() {
		t.Fatalf("source input directory was not recreated: %v", err)
	}
}

func TestCustomBuildSelectionUsesPersistedRecipeMode(t *testing.T) {
	if customBuild(map[string]any{}) {
		t.Fatal("Guided release selected container build")
	}
	if !customBuild(map[string]any{"pkgbuildManuallyModified": true}) {
		t.Fatal("Custom release did not select container build")
	}
}

func TestPatchProjectBuildPolicies(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	now := nowUTC()
	row, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "project-policy", DisplayName: "Policy", ArchPackageName: "policy-bin",
		SourceIdentity: "local:policy", HistoryJson: "[]", CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	ai := "ai"
	clear := "clear_after_success"
	service := &Service{DB: db}
	updated, err := service.PatchProject(ctx, row.ID, ProjectPatch{
		Revision: row.Revision, AutoBuildPolicy: &ai, CompileCachePolicy: &clear,
	})
	if err != nil {
		t.Fatal(err)
	}
	if updated.AutoBuildPolicy != ai || updated.CompileCachePolicy != clear {
		t.Fatalf("unexpected build policies: %+v", updated)
	}
	invalid := "sometimes"
	if _, err := service.PatchProject(ctx, row.ID, ProjectPatch{
		Revision: updated.Revision, AutoBuildPolicy: &invalid,
	}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("invalid build policy error = %v, want ErrInvalid", err)
	}
}

func TestAppendProjectHistoryIsServerOwnedAndPreservedByProjectPatches(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	now := nowUTC()
	row, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "project-history", DisplayName: "History", ArchPackageName: "history-bin",
		SourceIdentity: "local:history", HistoryJson: "[]", CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	service := &Service{DB: db}
	first, err := service.AppendProjectHistory(ctx, row.ID, "import", "Imported release 1.0")
	if err != nil {
		t.Fatal(err)
	}
	second, err := service.AppendProjectHistory(ctx, row.ID, "update-check", "No update available")
	if err != nil {
		t.Fatal(err)
	}
	if len(second.History) != 2 || second.History[0].Event != "import" ||
		second.History[1].Detail != "No update available" {
		t.Fatalf("history = %#v", second.History)
	}
	if second.Revision != first.Revision+1 {
		t.Fatalf("revision = %d, want %d", second.Revision, first.Revision+1)
	}
	second.DisplayName = "Renamed"
	patched, err := service.PatchProject(ctx, row.ID, ProjectPatch{
		Revision: second.Revision, DisplayName: second.DisplayName,
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(patched.History) != 2 || patched.History[1].Event != "update-check" {
		t.Fatalf("project patch discarded server history: %#v", patched.History)
	}
	operated, err := service.RecordPackageOperation(ctx, row.ID, "", "uninstall", 7, false, "pacman failed")
	if err != nil {
		t.Fatal(err)
	}
	if len(operated.History) != 3 || operated.History[2].Event != "uninstall" ||
		operated.History[2].Detail != "Uninstall of history-bin failed with exit code 7: pacman failed" {
		t.Fatalf("package-operation history = %#v", operated.History)
	}
	release, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID: "release-history", ProjectID: row.ID, State: "built", SourceType: "archive",
		VendorVersion: "1.0", OriginalFilename: "history-1.0.tar.gz",
		SourceSha256: strings.Repeat("a", 64), ArchPackageName: row.ArchPackageName,
		ArchPkgrel: 1, BodyJson: `{}`, CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := service.RecordBuildOutcome(ctx, release.ID, false, nil); err != nil {
		t.Fatal(err)
	}
	withBuild, err := service.GetProject(ctx, row.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(withBuild.History) != 4 || withBuild.History[3].Event != "build" ||
		withBuild.History[3].Detail != "Build of release 1.0 succeeded" {
		t.Fatalf("build history = %#v", withBuild.History)
	}
}

func TestBuildResultKeepsJobPayloadCompact(t *testing.T) {
	raw, err := json.Marshal(BuildResult{Status: "succeeded", Log: strings.Repeat("build output", 100)})
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(raw), "build output") {
		t.Fatalf("job result includes streamed build log: %s", raw)
	}
}

func TestReanalyzeAlignsIconWithExistingArchPackageName(t *testing.T) {
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

	png := []byte{
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
		0, 0, 0, 13, 'I', 'H', 'D', 'R',
		0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0, 0x90, 0x77, 0x53, 0xde,
		0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82,
	}
	var source bytes.Buffer
	tw := tar.NewWriter(&source)
	for name, contents := range map[string][]byte{
		"code-1.0/code.desktop": []byte("[Desktop Entry]\nName=Code\nExec=code\nIcon=vscode\n"),
		"code-1.0/vscode.png":   png,
	} {
		if err := tw.WriteHeader(&tar.Header{Name: name, Mode: 0o644, Size: int64(len(contents))}); err != nil {
			t.Fatal(err)
		}
		if _, err := tw.Write(contents); err != nil {
			t.Fatal(err)
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	artifactRecord, err := svc.Artifacts.Put(ctx, "code-1.0.tar", "source", bytes.NewReader(source.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	now := nowUTC()
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "proj-reanalyze-icon", DisplayName: "Code", ArchPackageName: "custom-code-bin",
		SourceIdentity: "local:code", HistoryJson: "[]", CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}
	release, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID: "rel-reanalyze-icon", ProjectID: project.ID, State: "needs-review", SourceType: "archive",
		VendorVersion: "1.0", OriginalFilename: "code-1.0.tar",
		SourceSha256: artifactRecord.SHA256, SourceArtifactID: sql.NullString{String: artifactRecord.ID, Valid: true},
		ArchPackageName: project.ArchPackageName, ArchPkgrel: 1,
		BodyJson:  `{"displayName":"Code","archPackageName":"custom-code-bin"}`,
		CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		t.Fatal(err)
	}

	if _, err := svc.Reanalyze(ctx, release.ID); err != nil {
		t.Fatal(err)
	}
	got, err := svc.GetRelease(ctx, release.ID)
	if err != nil {
		t.Fatal(err)
	}
	install, _ := mapValue(got.Document, "installMapping")
	icon, _ := mapValue(install, "icon")
	if stringValue(icon, "iconName") != "custom-code-bin" {
		t.Fatalf("reanalyzed icon name %q", stringValue(icon, "iconName"))
	}
	desktops := objectSlice(install["desktopEntries"])
	if len(desktops) != 1 || !strings.Contains(stringValue(desktops[0], "contents"), "Icon=custom-code-bin\n") {
		t.Fatalf("reanalyzed desktop entries %+v", desktops)
	}
	if !strings.Contains(stringValue(got.Document, "generatedPkgbuild"),
		"/usr/share/pixmaps/custom-code-bin.png") {
		t.Fatalf("reanalyzed PKGBUILD did not install the normalized icon")
	}
}

func TestListProjectSummariesOmitsReleaseDocuments(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	now := nowUTC()
	if _, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "proj-summary", DisplayName: "Summary", ArchPackageName: "summary-bin",
		SourceIdentity: "local:summary", HistoryJson: "[]", CreatedAt: now, ModifiedAt: now,
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID: "rel-summary", ProjectID: "proj-summary", State: "built", SourceType: "deb",
		VendorVersion: "2.4.1", OriginalFilename: "summary.deb",
		SourceSha256: strings.Repeat("a", 64), ArchPackageName: "summary-bin", ArchPkgrel: 3,
		BodyJson:  `{"debian":{"version":"2.4.1"},"lastBuildLog":"large log"}`,
		CreatedAt: now, ModifiedAt: now,
	}); err != nil {
		t.Fatal(err)
	}
	projects, err := (&Service{DB: db}).ListProjectSummaries(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(projects) != 1 || len(projects[0].Releases) != 1 {
		t.Fatalf("unexpected summaries: %+v", projects)
	}
	document := projects[0].Releases[0].Document
	if _, present := document["lastBuildLog"]; present {
		t.Fatal("summary included the full release document")
	}
	debian, ok := document["debian"].(map[string]any)
	if !ok || debian["version"] != "2.4.1" || document["archPkgrel"] != int64(3) {
		t.Fatalf("summary omitted version identity: %+v", document)
	}
}

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
	summaries, err := svc.ListProjectSummaries(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(summaries) != 1 || len(summaries[0].Releases) != 1 {
		t.Fatalf("unexpected project summaries: %+v", summaries)
	}
	iconSummary := summaries[0].Releases[0].Document
	if iconSummary["iconArtifactId"] != icon.ID {
		t.Fatalf("summary icon artifact = %v, want %q", iconSummary["iconArtifactId"], icon.ID)
	}
	if !releaseIconConfigured(iconSummary) {
		t.Fatalf("summary omitted configured icon metadata: %+v", iconSummary)
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

func TestReleaseSummaryIncludesUpdateHealthWithoutFullConfiguration(t *testing.T) {
	release := Release{Document: map[string]any{}}
	attachReleaseUpdateHealthSummary(&release, `{"update":{"strategy":"RPM repository",`+
		`"url":"https://packages.example.invalid","signingKeys":[{"contents":"large"}],`+
		`"lastChecked":"2026-08-28T21:06:45.510Z","lastCheckMessage":"missing key",`+
		`"lastCheckFailed":true,"lastAutomaticStatus":"paused",`+
		`"lastAutomaticMessage":"review required"}}`)
	update, ok := release.Document["update"].(map[string]any)
	if !ok || update["lastCheckFailed"] != true || update["lastCheckMessage"] != "missing key" ||
		update["lastAutomaticStatus"] != "paused" {
		t.Fatalf("update health summary = %+v", release.Document["update"])
	}
	if _, included := update["signingKeys"]; included {
		t.Fatalf("summary included signing key material: %+v", update)
	}
	if _, included := update["url"]; included {
		t.Fatalf("summary included full update configuration: %+v", update)
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
	if _, err := svc.PatchReleaseConfiguration(ctx, release.ID, updated.Revision,
		map[string]any{"history": []any{}}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("client history mutation error = %v, want ErrInvalid", err)
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
	withHistory, err := svc.GetProject(ctx, project.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(withHistory.History) != 1 || withHistory.History[0].Event != "release-discovered" ||
		!strings.Contains(withHistory.History[0].Detail, "150-105") {
		t.Fatalf("discovery history = %#v", withHistory.History)
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

func TestCleanupKeepsConfiguredNumberOfOutdatedVersionsAndPrunesArtifactsTogether(t *testing.T) {
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
	registry := &artifact.Registry{DB: db, Store: store}
	svc := &Service{DB: db, Artifacts: registry}
	if _, err := db.SQL.ExecContext(ctx, `UPDATE library_settings SET retention_versions = 1 WHERE id = 1`); err != nil {
		t.Fatal(err)
	}
	now := time.Now().UTC()
	stamp := func(daysAgo int) string {
		return now.Add(-time.Duration(daysAgo) * 24 * time.Hour).
			Truncate(time.Millisecond).Format("2006-01-02T15:04:05.000Z07:00")
	}
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "retention-project", DisplayName: "Retention App", ArchPackageName: "retention-bin",
		SourceIdentity: "local:retention", HistoryJson: "[]",
		CreatedAt: stamp(100), ModifiedAt: stamp(5),
	})
	if err != nil {
		t.Fatal(err)
	}
	type retainedRelease struct {
		id     string
		source artifact.Record
		built  artifact.Record
	}
	insertBuiltRelease := func(id, version string, createdDaysAgo, completedDaysAgo int) retainedRelease {
		t.Helper()
		source, putErr := registry.Put(ctx, id+".deb", "source", bytes.NewReader([]byte("source-"+id)))
		if putErr != nil {
			t.Fatal(putErr)
		}
		built, putErr := registry.Put(ctx, "retention-bin-"+version+"-1-x86_64.pkg.tar.zst",
			"arch_package", bytes.NewReader([]byte("built-"+id)))
		if putErr != nil {
			t.Fatal(putErr)
		}
		release, insertErr := db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
			ID: id, ProjectID: project.ID, State: "built", SourceType: "deb",
			VendorVersion: version, OriginalFilename: id + ".deb",
			SourceSha256: source.SHA256, SourceArtifactID: nullString(source.ID),
			ArchPackageName: project.ArchPackageName, ArchPkgrel: 1, BodyJson: `{}`,
			CreatedAt: stamp(createdDaysAgo), ModifiedAt: stamp(completedDaysAgo),
		})
		if insertErr != nil {
			t.Fatal(insertErr)
		}
		if insertErr = db.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
			ReleaseID: release.ID, ArtifactID: built.ID, Role: "built_package",
		}); insertErr != nil {
			t.Fatal(insertErr)
		}
		build, insertErr := db.Queries.InsertBuild(ctx, sqlcdb.InsertBuildParams{
			ID: "build-" + id, ReleaseID: release.ID, Status: "succeeded", LogText: "done",
			StartedAt:  nullString(stamp(completedDaysAgo)),
			FinishedAt: nullString(stamp(completedDaysAgo)),
		})
		if insertErr != nil {
			t.Fatal(insertErr)
		}
		if insertErr = db.Queries.InsertBuildArtifact(ctx, sqlcdb.InsertBuildArtifactParams{
			BuildID: build.ID, ArtifactID: built.ID,
		}); insertErr != nil {
			t.Fatal(insertErr)
		}
		return retainedRelease{id: release.ID, source: source, built: built}
	}
	pruned := insertBuiltRelease("release-old", "1.0", 90, 1)
	retained := insertBuiltRelease("release-recent", "2.0", 70, 90)
	stable := insertBuiltRelease("release-stable", "3.0", 60, 60)
	newest := insertBuiltRelease("release-newest", "4.0", 50, 50)
	if _, err := db.SQL.ExecContext(ctx, `UPDATE repo_settings SET stable_enabled = 1 WHERE id = 1`); err != nil {
		t.Fatal(err)
	}
	for _, pointer := range []struct {
		channel string
		release retainedRelease
		version string
	}{
		{channel: "stable", release: stable, version: "3.0"},
		{channel: "unstable", release: newest, version: "4.0"},
	} {
		if err := db.Queries.UpsertChannelEntry(ctx, sqlcdb.UpsertChannelEntryParams{
			Channel: pointer.channel, Arch: "x86_64", Pkgname: project.ArchPackageName,
			ProjectID: nullString(project.ID), ReleaseID: nullString(pointer.release.id),
			Pkgver: pointer.version, Pkgrel: "1", ArtifactID: pointer.release.built.ID,
			Filename:    project.ArchPackageName + "-" + pointer.version + "-1-x86_64.pkg.tar.zst",
			PublishedAt: stamp(0),
		}); err != nil {
			t.Fatal(err)
		}
	}

	if err := svc.Cleanup(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Queries.GetRelease(ctx, pruned.id); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("excess outdated release still exists: %v", err)
	}
	for _, artifactID := range []string{pruned.source.ID, pruned.built.ID} {
		if _, err := registry.Get(ctx, artifactID); err == nil {
			t.Fatalf("pruned artifact %s still exists", artifactID)
		}
	}
	for _, kept := range []retainedRelease{retained, stable, newest} {
		if _, err := db.Queries.GetRelease(ctx, kept.id); err != nil {
			t.Fatalf("retained release %s: %v", kept.id, err)
		}
		for _, artifactID := range []string{kept.source.ID, kept.built.ID} {
			if _, err := registry.Get(ctx, artifactID); err != nil {
				t.Fatalf("retained artifact %s: %v", artifactID, err)
			}
		}
	}
}
