package repo

import (
	"bytes"
	"context"
	"database/sql"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func requireRepoTools(t *testing.T) {
	t.Helper()
	for _, bin := range []string{"/usr/bin/gpg", "/usr/bin/repo-add", "/usr/bin/vercmp"} {
		if _, err := os.Stat(bin); err != nil {
			t.Skipf("missing %s", bin)
		}
	}
}

type repoFixture struct {
	svc  *Service
	db   *sqlite.DB
	now  time.Time
	ctx  context.Context
	root string
}

func newRepoFixture(t *testing.T) *repoFixture {
	t.Helper()
	requireRepoTools(t)
	t.Setenv("PACSMITH_SECRET_BACKEND", "file")
	root := t.TempDir()
	ctx := context.Background()
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
	now := time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)
	svc := New(db, &artifact.Registry{DB: db, Store: store}, secret.NewLockedStore(secret.BackendFile, fileStore),
		filepath.Join(root, "work"), filepath.Join(root, "gnupg"))
	svc.HostArch = "x86_64"
	fx := &repoFixture{svc: svc, db: db, now: now, ctx: ctx, root: root}
	svc.Now = func() time.Time { return fx.now }
	if _, err := svc.InitSigning(ctx); err != nil {
		t.Fatal(err)
	}
	return fx
}

func (fx *repoFixture) setSoak(t *testing.T, seconds int64) {
	t.Helper()
	zero := seconds
	if _, err := fx.svc.PatchSettings(fx.ctx, SettingsPatch{SoakSeconds: &zero}); err != nil {
		t.Fatal(err)
	}
}

func (fx *repoFixture) insertProject(t *testing.T, id, pkgname string) {
	t.Helper()
	stamp := fx.now.Format("2006-01-02T15:04:05.000Z07:00")
	if _, err := fx.db.Queries.InsertProject(fx.ctx, sqlcdb.InsertProjectParams{
		ID:              id,
		DisplayName:     pkgname,
		ArchPackageName: pkgname,
		SourceIdentity:  "local:" + id,
		HistoryJson:     "[]",
		CreatedAt:       stamp,
		ModifiedAt:      stamp,
	}); err != nil {
		t.Fatal(err)
	}
	publish := true
	if _, err := fx.svc.PatchProject(fx.ctx, id, ProjectPatch{Publish: &publish}); err != nil {
		t.Fatal(err)
	}
	if _, err := fx.db.Queries.InsertRelease(fx.ctx, sqlcdb.InsertReleaseParams{
		ID:               id + "-rel",
		ProjectID:        id,
		State:            "ready",
		SourceType:       "deb",
		VendorVersion:    "1.0.0",
		OriginalFilename: pkgname + ".deb",
		SourceSha256:     strings.Repeat("a", 64),
		ArchPackageName:  pkgname,
		ArchPkgrel:       1,
		BodyJson:         `{"debian":{"package":"` + pkgname + `"}}`,
		CreatedAt:        stamp,
		ModifiedAt:       stamp,
	}); err != nil {
		t.Fatal(err)
	}
}

func (fx *repoFixture) putPackage(t *testing.T, name, pkgver, pkgrel, arch string) string {
	t.Helper()
	body, err := WritePackage(PackageInfo{Name: name, Pkgver: pkgver, Pkgrel: pkgrel, Arch: arch}, nil, "")
	if err != nil {
		t.Fatal(err)
	}
	record, err := fx.svc.Artifacts.Put(fx.ctx, PackageFilename(PackageInfo{Name: name, Pkgver: pkgver, Pkgrel: pkgrel, Arch: arch}),
		"arch_package", bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	return record.ID
}

func (fx *repoFixture) publish(t *testing.T, projectID, pkgver, pkgrel string) string {
	t.Helper()
	id := fx.putPackage(t, projectPkgname(fx, projectID), pkgver, pkgrel, "x86_64")
	if err := fx.svc.PublishBuild(fx.ctx, projectID, projectID+"-rel", []string{id}); err != nil {
		t.Fatal(err)
	}
	return id
}

func projectPkgname(fx *repoFixture, projectID string) string {
	project, err := fx.db.Queries.GetProject(fx.ctx, projectID)
	if err != nil {
		return projectID
	}
	settings, _ := fx.svc.Settings(fx.ctx)
	name, _ := EffectiveName(project.ArchPackageName, originalName(project), settings.PackageNamePrefix, project.RepoPkgnameOverride)
	return name
}

func TestPublishableBuildBecomesUnstable(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 30*24*60*60)
	fx.insertProject(t, "proj-a", "demo-bin")
	id := fx.publish(t, "proj-a", "1.5.0", "1")
	status, err := fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Unstable == nil || status.Unstable.Artifact != id || status.Unstable.Pkgver != "1.5.0" {
		t.Fatalf("unstable %+v", status.Unstable)
	}
	if status.Stable != nil {
		t.Fatalf("stable should be empty: %+v", status.Stable)
	}
}

func TestFailedBuildLeavesRepoUnchanged(t *testing.T) {
	fx := newRepoFixture(t)
	fx.insertProject(t, "proj-a", "demo-bin")
	if err := fx.svc.PublishBuild(fx.ctx, "proj-a", "proj-a-rel", nil); err != nil {
		t.Fatal(err)
	}
	status, err := fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Unstable != nil || status.Stable != nil {
		t.Fatalf("empty/failed artifact list mutated repo: %+v %+v", status.Unstable, status.Stable)
	}
}

func TestIndependentSoakTimers(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 30*24*60*60)
	fx.insertProject(t, "proj-a", "demo-bin")
	fx.publish(t, "proj-a", "1.5.0", "1")
	fx.now = fx.now.Add(20 * 24 * time.Hour)
	fx.publish(t, "proj-a", "1.6.0", "1")
	fx.now = fx.now.Add(9 * 24 * time.Hour)
	if err := fx.svc.EvaluateSoaks(fx.ctx); err != nil {
		t.Fatal(err)
	}
	status, err := fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Stable != nil {
		t.Fatalf("nothing should be stable yet: %+v", status.Stable)
	}
	fx.now = fx.now.Add(24 * time.Hour)
	if err := fx.svc.EvaluateSoaks(fx.ctx); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Stable == nil || status.Stable.Pkgver != "1.5.0" {
		t.Fatalf("1.5.0 should be stable: %+v", status.Stable)
	}
	fx.now = fx.now.Add(20 * 24 * time.Hour)
	if err := fx.svc.EvaluateSoaks(fx.ctx); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Stable == nil || status.Stable.Pkgver != "1.6.0" {
		t.Fatalf("1.6.0 should be stable: %+v", status.Stable)
	}
}

func TestSameUpstreamRebuildResetsOnlyThatSoak(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 30*24*60*60)
	fx.insertProject(t, "proj-a", "demo-bin")
	old := fx.publish(t, "proj-a", "1.5.0", "1")
	fx.now = fx.now.Add(20 * 24 * time.Hour)
	fx.publish(t, "proj-a", "1.6.0", "1")
	fx.now = fx.now.Add(5 * 24 * time.Hour)
	newer := fx.publish(t, "proj-a", "1.5.0", "2")
	protected, err := fx.svc.ProtectedArtifactIDs(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := protected[old]; ok {
		t.Fatal("superseded 1.5.0-1 should not be a soak root")
	}
	if _, ok := protected[newer]; !ok {
		t.Fatal("1.5.0-2 should be protected")
	}
	soaks, err := fx.db.Queries.ListSoaks(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	var v15, v16 sqlcdb.RepoSoak
	for _, soak := range soaks {
		switch soak.Pkgver {
		case "1.5.0":
			v15 = soak
		case "1.6.0":
			v16 = soak
		}
	}
	if v15.Pkgrel != "2" || v15.SoakStartedAt == v16.SoakStartedAt {
		t.Fatalf("rebuild should reset only 1.5.0: %+v %+v", v15, v16)
	}
}

func TestMaturedCandidateAdvancesStableAndDoesNotDowngrade(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 0)
	fx.insertProject(t, "proj-a", "demo-bin")
	fx.publish(t, "proj-a", "1.6.0", "1")
	status, err := fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Stable == nil || status.Stable.Pkgver != "1.6.0" {
		t.Fatalf("stable %+v", status.Stable)
	}
	fx.setSoak(t, 30*24*60*60)
	fx.publish(t, "proj-a", "1.5.0", "2")
	fx.now = fx.now.Add(40 * 24 * time.Hour)
	if err := fx.svc.EvaluateSoaks(fx.ctx); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Stable == nil || status.Stable.Pkgver != "1.6.0" {
		t.Fatalf("older soak must not downgrade stable: %+v", status.Stable)
	}
}

func TestMultipleMaturedCandidatesAfterRestart(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 30*24*60*60)
	fx.insertProject(t, "proj-a", "demo-bin")
	fx.publish(t, "proj-a", "1.5.0", "1")
	fx.now = fx.now.Add(10 * 24 * time.Hour)
	fx.publish(t, "proj-a", "1.6.0", "1")
	fx.now = fx.now.Add(10 * 24 * time.Hour)
	fx.publish(t, "proj-a", "1.7.0", "1")
	fx.now = fx.now.Add(40 * 24 * time.Hour)
	if err := fx.svc.EvaluateSoaks(fx.ctx); err != nil {
		t.Fatal(err)
	}
	status, err := fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Stable == nil || status.Stable.Pkgver != "1.7.0" {
		t.Fatalf("restart catch-up should pick newest eligible: %+v", status.Stable)
	}
}

func TestManualPromotion(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 30*24*60*60)
	fx.insertProject(t, "proj-a", "demo-bin")
	fx.publish(t, "proj-a", "1.8.0", "1")
	status, err := fx.svc.Promote(fx.ctx, "proj-a", "", "")
	if err != nil {
		t.Fatal(err)
	}
	if status.Stable == nil || status.Stable.Pkgver != "1.8.0" {
		t.Fatalf("manual promote: %+v", status.Stable)
	}
}

func TestDuplicateEffectiveNameRejected(t *testing.T) {
	fx := newRepoFixture(t)
	fx.insertProject(t, "proj-a", "shared-bin")
	fx.publish(t, "proj-a", "1.0.0", "1")
	stamp := fx.now.Format("2006-01-02T15:04:05.000Z07:00")
	if _, err := fx.db.Queries.InsertProject(fx.ctx, sqlcdb.InsertProjectParams{
		ID:              "proj-b",
		DisplayName:     "Other",
		ArchPackageName: "shared-bin",
		SourceIdentity:  "local:proj-b",
		HistoryJson:     "[]",
		CreatedAt:       stamp,
		ModifiedAt:      stamp,
	}); err != nil {
		t.Fatal(err)
	}
	publish := true
	if _, err := fx.svc.PatchProject(fx.ctx, "proj-b", ProjectPatch{Publish: &publish}); err == nil {
		t.Fatal("expected package name collision")
	}
}

func TestPrefixAndOverrideNames(t *testing.T) {
	fx := newRepoFixture(t)
	prefix := "pacsmith-"
	if _, err := fx.svc.PatchSettings(fx.ctx, SettingsPatch{PackageNamePrefix: &prefix}); err != nil {
		t.Fatal(err)
	}
	fx.insertProject(t, "proj-a", "slack-desktop-bin")
	status, err := fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.EffectivePackageName != "pacsmith-slack-desktop-bin" {
		t.Fatalf("prefix default %q", status.EffectivePackageName)
	}
	override := "acme-slack"
	if _, err := fx.svc.PatchProject(fx.ctx, "proj-a", ProjectPatch{Override: &override}); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.EffectivePackageName != "acme-slack" {
		t.Fatalf("override %q", status.EffectivePackageName)
	}
}

func TestSigningAndRepositoryDatabases(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 0)
	fx.insertProject(t, "proj-a", "demo-bin")
	fx.publish(t, "proj-a", "1.0.0", "1")
	settings, err := fx.svc.Settings(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	if settings.Fingerprint == "" || len(settings.Fingerprint) < 40 {
		t.Fatalf("fingerprint %q", settings.Fingerprint)
	}
	unstable, err := fx.db.Queries.GetRepoDatabase(fx.ctx, sqlcdb.GetRepoDatabaseParams{Channel: ChannelUnstable, Arch: "x86_64"})
	if err != nil {
		t.Fatal(err)
	}
	if !unstable.DbSigArtifactID.Valid {
		t.Fatal("unstable repository database is unsigned")
	}
	stable, err := fx.db.Queries.GetRepoDatabase(fx.ctx, sqlcdb.GetRepoDatabaseParams{Channel: ChannelStable, Arch: "x86_64"})
	if err != nil {
		t.Fatal(err)
	}
	if !stable.DbSigArtifactID.Valid {
		t.Fatal("stable repository database is unsigned")
	}
	trusted, err := fx.db.Queries.GetRepoSettings(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	body, err := fx.readArtifact(trusted.KeyringTrustedArtifactID)
	if err != nil {
		t.Fatal(err)
	}
	fp := NormalizeFingerprint(settings.Fingerprint)
	if !strings.Contains(string(body), fp+":4:") {
		t.Fatalf("direct-trust keyring missing fingerprint: %s", body)
	}
}

func TestRootCertifiedVerification(t *testing.T) {
	fx := newRepoFixture(t)
	rootHome := filepath.Join(fx.root, "root-gnupg")
	if err := os.MkdirAll(rootHome, 0o700); err != nil {
		t.Fatal(err)
	}
	if _, err := runGPGHome(fx.ctx, rootHome, nil, "--quick-generate-key",
		"Example Org Root <security@example.com>", "ed25519", "default", "never"); err != nil {
		t.Fatal(err)
	}
	rootPub, err := runGPGHome(fx.ctx, rootHome, nil, "--export", "--armor")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := fx.svc.UploadRootKey(fx.ctx, rootPub); err != nil {
		t.Fatal(err)
	}
	pacsmithPub, _, err := fx.svc.PublicKey(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runGPGHome(fx.ctx, rootHome, pacsmithPub, "--import"); err != nil {
		t.Fatal(err)
	}
	settings, err := fx.svc.Settings(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runGPGHome(fx.ctx, rootHome, nil, "--quick-sign-key", settings.Fingerprint); err != nil {
		t.Fatal(err)
	}
	certified, err := runGPGHome(fx.ctx, rootHome, nil, "--export", "--armor", settings.Fingerprint)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := fx.svc.UploadCertifiedKey(fx.ctx, certified); err != nil {
		t.Fatal(err)
	}
	wrongHome := filepath.Join(fx.root, "wrong-gnupg")
	if err := os.MkdirAll(wrongHome, 0o700); err != nil {
		t.Fatal(err)
	}
	if _, err := runGPGHome(fx.ctx, wrongHome, nil, "--quick-generate-key",
		"Wrong Key <wrong@example.com>", "ed25519", "default", "never"); err != nil {
		t.Fatal(err)
	}
	wrongPub, err := runGPGHome(fx.ctx, wrongHome, nil, "--export", "--armor")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := fx.svc.UploadCertifiedKey(fx.ctx, wrongPub); err == nil {
		t.Fatal("accepted a certified key that is not the PacSmith signing key")
	}
	otherRoot := filepath.Join(fx.root, "other-root")
	if err := os.MkdirAll(otherRoot, 0o700); err != nil {
		t.Fatal(err)
	}
	if _, err := runGPGHome(fx.ctx, otherRoot, nil, "--quick-generate-key",
		"Other Root <other@example.com>", "ed25519", "default", "never"); err != nil {
		t.Fatal(err)
	}
	if _, err := runGPGHome(fx.ctx, otherRoot, pacsmithPub, "--import"); err != nil {
		t.Fatal(err)
	}
	if _, err := runGPGHome(fx.ctx, otherRoot, nil, "--quick-sign-key", settings.Fingerprint); err != nil {
		t.Fatal(err)
	}
	otherCertified, err := runGPGHome(fx.ctx, otherRoot, nil, "--export", "--armor", settings.Fingerprint)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := fx.svc.UploadCertifiedKey(fx.ctx, otherCertified); err == nil {
		t.Fatal("accepted certification from the wrong root")
	}
}

func TestTransactionalPublishFailurePreservesState(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 30*24*60*60)
	fx.insertProject(t, "proj-a", "demo-bin")
	first := fx.publish(t, "proj-a", "1.0.0", "1")
	before, err := fx.db.Queries.GetRepoDatabase(fx.ctx, sqlcdb.GetRepoDatabaseParams{Channel: ChannelUnstable, Arch: "x86_64"})
	if err != nil {
		t.Fatal(err)
	}
	savedWork := fx.svc.WorkDir
	fx.svc.WorkDir = filepath.Join("/proc", "does-not-exist", "repo")
	second := fx.putPackage(t, "demo-bin", "2.0.0", "1", "x86_64")
	if err := fx.svc.PublishBuild(fx.ctx, "proj-a", "proj-a-rel", []string{second}); err == nil {
		t.Fatal("expected publish failure")
	}
	fx.svc.WorkDir = savedWork
	after, err := fx.db.Queries.GetRepoDatabase(fx.ctx, sqlcdb.GetRepoDatabaseParams{Channel: ChannelUnstable, Arch: "x86_64"})
	if err != nil {
		t.Fatal(err)
	}
	if after.DbArtifactID != before.DbArtifactID {
		t.Fatalf("failed publish replaced repository database %s -> %s", before.DbArtifactID, after.DbArtifactID)
	}
	status, err := fx.svc.ProjectView(fx.ctx, "proj-a")
	if err != nil {
		t.Fatal(err)
	}
	if status.Unstable == nil || status.Unstable.Artifact != first {
		t.Fatalf("failed publish mutated unstable: %+v", status.Unstable)
	}
}

func TestGCProtectsRepoRootsAndReleasesAfterRemoval(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 30*24*60*60)
	fx.insertProject(t, "proj-a", "demo-bin")
	stableID := fx.publish(t, "proj-a", "1.4.0", "1")
	fx.now = fx.now.Add(40 * 24 * time.Hour)
	if err := fx.svc.EvaluateSoaks(fx.ctx); err != nil {
		t.Fatal(err)
	}
	old := fx.publish(t, "proj-a", "1.5.0", "1")
	mid := fx.publish(t, "proj-a", "1.6.0", "1")
	unstable := fx.publish(t, "proj-a", "1.7.0", "1")
	protected, err := fx.svc.ProtectedArtifactIDs(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{stableID, old, mid, unstable} {
		if _, ok := protected[id]; !ok {
			t.Fatalf("missing GC root %s", id)
		}
	}
	replaced := fx.publish(t, "proj-a", "1.5.0", "2")
	protected, err = fx.svc.ProtectedArtifactIDs(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := protected[old]; ok {
		t.Fatal("superseded soak still protected")
	}
	if _, ok := protected[replaced]; !ok {
		t.Fatal("replacement soak not protected")
	}
	orphan, err := fx.svc.Artifacts.Put(fx.ctx, "orphan.bin", "unknown", bytes.NewReader([]byte("orphan")))
	if err != nil {
		t.Fatal(err)
	}
	protected, err = fx.svc.ProtectedArtifactIDs(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := protected[orphan.ID]; ok {
		t.Fatal("unreferenced artifact should not be a repo GC root")
	}
	if err := fx.db.Queries.DeleteProject(fx.ctx, "proj-a"); err != nil {
		t.Fatal(err)
	}
	if err := fx.svc.OnProjectDeleted(fx.ctx, "proj-a"); err != nil {
		t.Fatal(err)
	}
	protected, err = fx.svc.ProtectedArtifactIDs(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := protected[unstable]; ok {
		t.Fatal("deleted project packages should no longer be repo GC roots")
	}
}

func TestRepoHTTPAndKeyringPackage(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 0)
	fx.insertProject(t, "proj-a", "demo-bin")
	fx.publish(t, "proj-a", "1.0.0", "1")
	server := httptest.NewServer(fx.svc.Handler())
	t.Cleanup(server.Close)
	resp, err := http.Get(server.URL + "/repo/unstable/x86_64/pacsmith.db")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("unstable db status %d", resp.StatusCode)
	}
	resp, err = http.Get(server.URL + "/repo/stable/x86_64/pacsmith.db")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("stable db status %d", resp.StatusCode)
	}
	resp, err = http.Get(server.URL + "/bootstrap/pacsmith.gpg")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("bootstrap keyring status %d", resp.StatusCode)
	}
	entry, err := fx.db.Queries.GetChannelEntry(fx.ctx, sqlcdb.GetChannelEntryParams{
		Channel: ChannelStable,
		Arch:    "any",
		Pkgname: KeyringPackage,
	})
	if err != nil {
		t.Fatal(err)
	}
	if entry.Pkgname != KeyringPackage {
		t.Fatalf("keyring entry %+v", entry)
	}
}

func (fx *repoFixture) readArtifact(id sql.NullString) ([]byte, error) {
	if !id.Valid {
		return nil, sql.ErrNoRows
	}
	_, file, err := fx.svc.Artifacts.Open(fx.ctx, id.String)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	var buf bytes.Buffer
	if _, err := buf.ReadFrom(file); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func TestVercmpAdvances(t *testing.T) {
	requireRepoTools(t)
	ok, err := Advances(0, "1.6.0", "1", 0, "1.5.0", "2")
	if err != nil || !ok {
		t.Fatalf("1.6.0-1 should advance 1.5.0-2: %v %v", ok, err)
	}
	ok, err = Advances(0, "1.5.0", "2", 0, "1.6.0", "1")
	if err != nil || ok {
		t.Fatalf("older version must not advance: %v %v", ok, err)
	}
}
