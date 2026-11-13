package repo

import (
	"testing"
	"time"
)

func TestManualPromotionDoesNotStartAutomaticSoaks(t *testing.T) {
	fx := newRepoFixture(t)
	fx.setSoak(t, 3600)
	fx.insertProject(t, "project", "demo-bin")
	automatic := false
	if _, err := fx.svc.PatchProject(fx.ctx, "project", ProjectPatch{AutomaticSoak: &automatic}); err != nil {
		t.Fatal(err)
	}
	fx.publish(t, "project", "1.0", "1")
	fx.publish(t, "project", "2.0", "1")
	soaks, err := fx.db.Queries.ListSoaks(fx.ctx)
	if err != nil || len(soaks) != 0 {
		t.Fatalf("manual builds started automatic timers: %+v, %v", soaks, err)
	}
	fx.now = fx.now.Add(24 * time.Hour)
	if err := fx.svc.EvaluateSoaks(fx.ctx); err != nil {
		t.Fatal(err)
	}
	status, err := fx.svc.ProjectView(fx.ctx, "project")
	if err != nil || status.Stable != nil {
		t.Fatalf("manual project promoted automatically: %+v, %v", status, err)
	}
	status, err = fx.svc.Promote(fx.ctx, "project", "", "x86_64")
	if err != nil || status.Stable == nil || status.Stable.Pkgver != "2.0" {
		t.Fatalf("manual promotion of current Unstable stopped working: %+v, %v", status, err)
	}
}
