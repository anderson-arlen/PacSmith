package updatecheck

import (
	"context"
	"encoding/json"
	"errors"
	"path/filepath"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestAutomaticReviewHandoff(t *testing.T) {
	for _, tc := range []struct {
		name, policy, status, want string
		custom                     bool
	}{
		{"AI resolves review blockers", "ai", "", "ai-pending", false},
		{"AI resolves custom recipes", "ai", "", "ai-pending", true},
		{"existing harness is not relaunched", "ai", "ai-reviewing", "ai-reviewing", true},
		{"review-free still pauses", "review_free", "", "paused", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx := context.Background()
			db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "library.db"))
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = db.Close() })
			_, err = db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
				ID: "project", DisplayName: "Demo", ArchPackageName: "demo", HistoryJson: "[]",
			})
			if err != nil {
				t.Fatal(err)
			}
			service := &Service{DB: db, Library: &library.Service{DB: db}}
			project, err := service.Library.PatchProject(ctx, "project", library.ProjectPatch{AutoBuildPolicy: &tc.policy})
			if err != nil {
				t.Fatal(err)
			}
			document := reviewFixture("libnew")
			document["buildStatus"] = "never-built"
			document["builtArtifactIds"] = []any{}
			document["pkgbuildManuallyModified"] = tc.custom
			document["update"] = map[string]any{"lastAutomaticStatus": tc.status}
			raw, err := json.Marshal(document)
			if err != nil {
				t.Fatal(err)
			}
			_, err = db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
				ID: "candidate", ProjectID: project.ID, State: "needs-review", SourceType: "deb",
				VendorVersion: "2.0", SourceSha256: "candidate", ArchPackageName: "demo",
				ArchPkgrel: 1, BodyJson: string(raw),
			})
			if err != nil {
				t.Fatal(err)
			}
			target := checkTarget{Project: project, Release: library.Release{Document: reviewFixture("libold")}}
			built, err := service.buildIfReviewFree(ctx, target, "candidate", func(string) {})
			if built || err == nil {
				t.Fatalf("built = %v, error = %v", built, err)
			}
			result := Result{}
			setAutomaticBuildError(&result, err)
			if result.AutomaticStatus != tc.want {
				t.Fatalf("outcome = %+v, want %s", result, tc.want)
			}
			if err := service.persistAutomaticOutcome(ctx, "candidate", result); err != nil {
				t.Fatal(err)
			}
			release, err := service.Library.GetRelease(ctx, "candidate")
			if err != nil || stringValue(object(release.Document["update"]), "lastAutomaticStatus") != tc.want {
				t.Fatalf("persisted release = %+v, error = %v", release, err)
			}
			// Reconciliation must also hand off already prepared releases when no new
			// version is found, including projects without a successful baseline build.
			_, err = db.Queries.UpdateLibrarySettings(ctx, sqlcdb.UpdateLibrarySettingsParams{
				Revision: 1, AiProvider: "none", AiReasoningEffort: "provider-default", AiExecutionMode: "standard",
				UpdatesAutoPrepare: 1, UpdatesWeekday: 1, RetentionVersions: 2, BuildParallelism: 1,
			})
			if err != nil {
				t.Fatal(err)
			}
			result = service.reconcilePreparedBuild(ctx, project.ID, Result{Status: "no-update"}, func(string) {})
			if result.AutomaticStatus != tc.want || !result.Prepared {
				t.Fatalf("reconciled outcome = %+v, want %s", result, tc.want)
			}
			if tc.status == "ai-reviewing" {
				if err := service.persistAutomaticOutcome(ctx, "candidate", Result{AutomaticStatus: "ai-pending"}); err != nil {
					t.Fatal(err)
				}
				release, err := service.Library.GetRelease(ctx, "candidate")
				if err != nil || stringValue(object(release.Document["update"]), "lastAutomaticStatus") != "ai-reviewing" {
					t.Fatalf("late check overwrote the harness claim: %+v, error = %v", release, err)
				}
			}
		})
	}
}

func TestActualBuildFailureStillPauses(t *testing.T) {
	result := Result{}
	setAutomaticBuildError(&result, errors.New("build failed"))
	if result.AutomaticStatus != "paused" || result.AutomaticMessage != "build failed" {
		t.Fatalf("outcome = %+v", result)
	}
}
