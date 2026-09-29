package repo

import (
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestNewProjectRepositoryDefaults(t *testing.T) {
	fx := newRepoFixture(t)
	insert := func(id string) {
		t.Helper()
		stamp := fx.svc.nowString()
		if _, err := fx.db.Queries.InsertProject(fx.ctx, sqlcdb.InsertProjectParams{
			ID: id, DisplayName: id, ArchPackageName: id, SourceIdentity: "local:" + id,
			HistoryJson: "[]", CreatedAt: stamp, ModifiedAt: stamp,
		}); err != nil {
			t.Fatal(err)
		}
	}
	insert("disabled")
	status, err := fx.svc.ProjectView(fx.ctx, "disabled")
	if err != nil || !status.Publish || !status.AutomaticSoak {
		t.Fatalf("projects must inherit publication even before the repository is enabled: %+v, %v", status, err)
	}
	override := ""
	if _, err := fx.svc.PatchProject(fx.ctx, "disabled", ProjectPatch{Override: &override}); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "disabled")
	if err != nil || !status.AutomaticSoak || status.StableChannelEnabled {
		t.Fatalf("saving while Stable is disabled must preserve automatic promotion: %+v, %v", status, err)
	}
	fx.setSoak(t, 3600)
	settings, err := fx.db.Queries.GetRepoSettings(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	update := updateParamsFrom(settings, settings.Revision)
	update.Enabled = 1
	if _, err := fx.db.Queries.UpdateRepoSettings(fx.ctx, update); err != nil {
		t.Fatal(err)
	}
	insert("enabled")
	status, err = fx.svc.ProjectView(fx.ctx, "enabled")
	if err != nil || !status.Publish || !status.AutomaticSoak || status.SoakSecondsOverride != -1 {
		t.Fatalf("enabled repository defaults: %+v, %v", status, err)
	}
	stamp := fx.svc.nowString()
	if _, err := fx.db.Queries.InsertRelease(fx.ctx, sqlcdb.InsertReleaseParams{
		ID: "enabled-rel", ProjectID: "enabled", State: "ready", SourceType: "deb",
		VendorVersion: "1.0.0", ArchPackageName: "enabled", ArchPkgrel: 1,
		BodyJson: `{}`, CreatedAt: stamp, ModifiedAt: stamp,
	}); err != nil {
		t.Fatal(err)
	}
	artifactID := fx.publish(t, "enabled", "1.0.0", "1")
	if _, err := fx.db.Queries.InsertBuild(fx.ctx, sqlcdb.InsertBuildParams{
		ID: "build", ReleaseID: "enabled-rel", Status: "succeeded",
	}); err != nil {
		t.Fatal(err)
	}
	if err := fx.db.Queries.InsertBuildArtifact(fx.ctx, sqlcdb.InsertBuildArtifactParams{
		BuildID: "build", ArtifactID: artifactID,
	}); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "enabled")
	if err != nil || status.Unstable == nil || status.Stable != nil {
		t.Fatalf("new builds must first appear in Unstable: %+v, %v", status, err)
	}
	fx.now = fx.now.Add(time.Hour)
	if err := fx.svc.EvaluateSoaks(fx.ctx); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "enabled")
	if err != nil || status.Stable == nil || status.Stable.Pkgver != "1.0.0" {
		t.Fatalf("default promotion must advance Stable after the inherited soak: %+v, %v", status, err)
	}
	stable := false
	if _, err := fx.svc.PatchSettings(fx.ctx, SettingsPatch{StableEnabled: &stable}); err != nil {
		t.Fatal(err)
	}
	if _, err := fx.svc.PatchProject(fx.ctx, "enabled", ProjectPatch{Override: &override}); err != nil {
		t.Fatal(err)
	}
	stable = true
	if _, err := fx.svc.PatchSettingsDeferred(fx.ctx, SettingsPatch{StableEnabled: &stable}); err != nil {
		t.Fatal(err)
	}
	if err := fx.svc.ReconcileAllDistribution(fx.ctx); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "enabled")
	if err != nil || !status.AutomaticSoak || len(status.Soaks) != 1 {
		t.Fatalf("enabling Stable must resume soaking retained builds: %+v, %v", status, err)
	}
	manual := false
	if _, err := fx.svc.PatchProject(fx.ctx, "enabled", ProjectPatch{AutomaticSoak: &manual}); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "enabled")
	if err != nil || status.AutomaticSoak || !status.Publish {
		t.Fatalf("explicit manual promotion must remain available: %+v, %v", status, err)
	}
	if _, err := fx.svc.PatchProject(fx.ctx, "enabled", ProjectPatch{Publish: &manual}); err != nil {
		t.Fatal(err)
	}
	status, err = fx.svc.ProjectView(fx.ctx, "enabled")
	if err != nil || status.Publish {
		t.Fatalf("explicit publication opt-out must remain available: %+v, %v", status, err)
	}
}

func TestBuildBeforeRepositorySigningIsConfigured(t *testing.T) {
	fx := newRepoFixture(t)
	fx.insertProject(t, "project", "demo-bin")
	settings, err := fx.db.Queries.GetRepoSettings(fx.ctx)
	if err != nil {
		t.Fatal(err)
	}
	update := updateParamsFrom(settings, settings.Revision)
	update.SigningInitialized = 0
	if _, err := fx.db.Queries.UpdateRepoSettings(fx.ctx, update); err != nil {
		t.Fatal(err)
	}
	prep, err := fx.svc.PrepareBuild(fx.ctx, "project", "project-rel")
	if err != nil || prep.Publish || prep.PackageName != "demo-bin" {
		t.Fatalf("local builds must work before repository setup: %+v, %v", prep, err)
	}
	fx.publish(t, "project", "1.0", "1")
	status, err := fx.svc.ProjectView(fx.ctx, "project")
	if err != nil || !status.Publish || status.Unstable != nil {
		t.Fatalf("publication must wait for signing without opting the project out: %+v, %v", status, err)
	}
}
