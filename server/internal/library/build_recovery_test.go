package library

import (
	"context"
	"encoding/json"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestRecoverStaleBuildState(t *testing.T) {
	for _, scenario := range []string{"orphan", "interrupted", "queued", "completed", "older-success"} {
		t.Run(scenario, func(t *testing.T) {
			ctx := context.Background()
			db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "library.db"))
			if err != nil {
				t.Fatal(err)
			}
			defer db.Close()
			svc := &Service{DB: db}
			base := time.Now().UTC().Add(-time.Hour)
			stamp := func(minutes int) string {
				return base.Add(time.Duration(minutes) * time.Minute).Format(time.RFC3339Nano)
			}
			_, err = db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{ID: "project", DisplayName: "Test", ArchPackageName: "test-bin", SourceIdentity: "test", HistoryJson: "[]", CreatedAt: stamp(0), ModifiedAt: stamp(0)})
			if err != nil {
				t.Fatal(err)
			}
			_, err = db.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{ID: "release", ProjectID: "project", State: "needs-review", SourceType: "deb", VendorVersion: "1", OriginalFilename: "test.deb", SourceSha256: strings.Repeat("a", 64), ArchPackageName: "test-bin", ArchPkgrel: 1, BodyJson: `{"buildStatus":"building","lastBuildLog":"retained output"}`, CreatedAt: stamp(0), ModifiedAt: stamp(0)})
			if err != nil {
				t.Fatal(err)
			}
			if scenario != "orphan" {
				status := "interrupted"
				if scenario == "queued" {
					status = "queued"
				}
				_, err := db.Queries.InsertJob(ctx, sqlcdb.InsertJobParams{ID: "job", Kind: "build", Status: status, ProjectID: nullString("project"), ReleaseID: nullString("release"), PayloadJson: `{}`, CreatedAt: stamp(10)})
				if err != nil {
					t.Fatal(err)
				}
				if status == "interrupted" {
					_, err = db.Queries.UpdateJob(ctx, sqlcdb.UpdateJobParams{ID: "job", Status: status, ProjectID: nullString("project"), ReleaseID: nullString("release"), StartedAt: nullString(stamp(11)), FinishedAt: nullString(stamp(15))})
					if err != nil {
						t.Fatal(err)
					}
				}
			}
			if scenario == "completed" || scenario == "older-success" {
				minute := 12
				if scenario == "older-success" {
					minute = 2
				}
				_, err := db.Queries.InsertBuild(ctx, sqlcdb.InsertBuildParams{ID: "success", ReleaseID: "release", Status: "succeeded", LogText: "completed output", StartedAt: nullString(stamp(minute)), FinishedAt: nullString(stamp(minute + 1))})
				if err != nil {
					t.Fatal(err)
				}
			}
			readLog := func(string) (string, error) { return "historical output\n", nil }
			if err := svc.RecoverInterruptedBuilds(ctx, readLog); err != nil {
				t.Fatal(err)
			}
			row, err := db.Queries.GetRelease(ctx, "release")
			if err != nil {
				t.Fatal(err)
			}
			var body map[string]any
			if err := json.Unmarshal([]byte(row.BodyJson), &body); err != nil {
				t.Fatal(err)
			}
			want := "failed"
			if scenario == "queued" {
				want = "building"
			}
			if scenario == "completed" {
				want = "succeeded"
			}
			if body["buildStatus"] != want {
				t.Fatalf("state = %v, want %s", body["buildStatus"], want)
			}
			builds, err := db.Queries.ListBuildsForRelease(ctx, "release")
			if err != nil {
				t.Fatal(err)
			}
			if want == "failed" {
				last := builds[len(builds)-1]
				if last.Status != "failed" || !strings.Contains(last.LogText, "output") || !strings.Contains(last.LogText, "daemon restart") {
					t.Fatalf("recovered build %+v", last)
				}
				if scenario != "orphan" && last.StartedAt.String != stamp(11) {
					t.Fatalf("lost original start: %+v", last)
				}
			}
			if err := svc.RecoverInterruptedBuilds(ctx, readLog); err != nil {
				t.Fatal(err)
			}
			again, err := db.Queries.ListBuildsForRelease(ctx, "release")
			if err != nil || len(again) != len(builds) {
				t.Fatal("recovery duplicated history")
			}
		})
	}
}
