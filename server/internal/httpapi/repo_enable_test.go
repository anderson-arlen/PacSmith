package httpapi

import (
	"bytes"
	"context"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/listen"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestEnablingRepositoryQueuesPublishedProjectsAfterBinding(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	row, err := db.Queries.GetRepoSettings(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Queries.UpdateRepoSettings(ctx, sqlcdb.UpdateRepoSettingsParams{
		Enabled: row.Enabled, ListenHosts: row.ListenHosts, ListenPort: row.ListenPort,
		AdvertisedUrl: row.AdvertisedUrl, StableEnabled: row.StableEnabled,
		SoakSeconds: row.SoakSeconds, PackageNamePrefix: row.PackageNamePrefix,
		TrustMode: row.TrustMode, SigningFingerprint: "replacement-key",
		SigningInitialized: 1, SigningPubkeyArtifactID: row.SigningPubkeyArtifactID,
		RootPubkeyArtifactID: row.RootPubkeyArtifactID, RootFingerprint: row.RootFingerprint,
		CertifiedPubkeyArtifactID:   row.CertifiedPubkeyArtifactID,
		KeyringGpgArtifactID:        row.KeyringGpgArtifactID,
		KeyringTrustedArtifactID:    row.KeyringTrustedArtifactID,
		KeyringRevokedArtifactID:    row.KeyringRevokedArtifactID,
		KeyringPackageArtifactID:    row.KeyringPackageArtifactID,
		KeyringPackageSigArtifactID: row.KeyringPackageSigArtifactID,
		KeyringVersion:              row.KeyringVersion, RecoveryMessage: "key was replaced",
		ModifiedAt: "2026-01-01T00:00:00Z", Revision: row.Revision,
	}); err != nil {
		t.Fatal(err)
	}
	project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "project-1", DisplayName: "Demo", ArchPackageName: "demo-bin",
		SourceIdentity: "local:demo", HistoryJson: "[]",
		CreatedAt: "2026-01-01T00:00:00Z", ModifiedAt: "2026-01-01T00:00:00Z",
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Queries.UpdateProjectRepo(ctx, sqlcdb.UpdateProjectRepoParams{
		RepoPublish: 1, RepoPkgnameOverride: "", RepoPublishedPkgname: "",
		ModifiedAt: "2026-01-01T00:00:01Z", ID: project.ID, Revision: project.Revision,
	}); err != nil {
		t.Fatal(err)
	}

	manager, err := jobs.New(db, filepath.Join(root, "jobs"), nil)
	if err != nil {
		t.Fatal(err)
	}
	bound := false
	server := &Server{Config: Config{
		DB: db, Repo: repo.New(db, nil, nil, filepath.Join(root, "repo"), filepath.Join(root, "gnupg")),
		Jobs: manager,
		ApplyRepo: func(config listen.Config) error {
			bound = config.Enabled
			return nil
		},
	}}
	req := httptest.NewRequest(http.MethodPatch, "/api/v1/repo", bytes.NewBufferString(`{"enabled":true}`))
	recorder := httptest.NewRecorder()
	server.patchRepo(recorder, req)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status %d: %s", recorder.Code, recorder.Body.String())
	}
	if !bound {
		t.Fatal("repository listener was not enabled immediately")
	}
	active, err := manager.Active(ctx, jobs.KindRepositoryDistribution)
	if err != nil {
		t.Fatal(err)
	}
	if len(active) != 1 || active[0].ProjectID != project.ID {
		t.Fatalf("queued repository jobs: %+v", active)
	}
}
