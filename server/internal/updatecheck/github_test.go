package updatecheck

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"

	githubapi "github.com/anderson-arlen/pacsmith/server/internal/github"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestGitHubCheckUsesAcquiredReleaseInsteadOfLastObservation(t *testing.T) {
	for _, tc := range []struct {
		name, version     string
		releaseID, seenID int64
		prerelease        bool
		wantUpdate        bool
	}{
		{"legacy prerelease", "2.0.0_rc.4", 0, 20, true, true},
		{"known prerelease", "2.0.0_rc.4", 10, 20, true, true},
		{"unprepared stable successor", "1.0.0", 10, 20, false, true},
		{"acquired release with different artifact version", "1.0.0", 20, 30, false, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx := context.Background()
			payload := archiveFixture(t)
			downloads := 0
			client := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
				var body io.Reader
				if request.URL.Host == "api.github.com" {
					body = strings.NewReader(`[{"id":20,"tag_name":"v2.0.0","assets":[{"id":201,"name":"demo-2.0.tar","browser_download_url":"https://vendor.invalid/demo-2.0.tar"}]}]`)
				} else {
					downloads++
					body = bytes.NewReader(payload)
				}
				return &http.Response{StatusCode: http.StatusOK, Header: make(http.Header),
					Body: io.NopCloser(body), Request: request}, nil
			})}
			service, lib, cleanup := remoteImportFixture(t, client)
			defer cleanup()
			service.GitHub = &githubapi.Service{Client: client, Artifacts: service.Artifacts}
			_, err := service.DB.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
				ID: "project", DisplayName: "Demo", ArchPackageName: "demo", HistoryJson: "[]",
			})
			if err != nil {
				t.Fatal(err)
			}
			document := map[string]any{
				"debian": map[string]any{"version": tc.version},
				"acquisition": map[string]any{
					"kind": "github-release", "githubReleaseId": tc.releaseID,
					"githubAssetId": tc.releaseID*10 + 1, "githubPrerelease": tc.prerelease,
				},
				"update": map[string]any{
					"strategy": StrategyGitHub, "githubOwner": "vendor", "githubRepository": "demo",
					"githubAssetRegex": `demo-.*\.tar`,
					"githubReleaseId":  tc.seenID, "githubAssetId": tc.seenID*10 + 1,
				},
			}
			raw, err := json.Marshal(document)
			if err != nil {
				t.Fatal(err)
			}
			_, err = service.DB.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
				ID: "current", ProjectID: "project", State: "built", SourceType: "archive",
				VendorVersion: tc.version, SourceSha256: "current", ArchPackageName: "demo",
				ArchPkgrel: 1, BodyJson: string(raw),
			})
			if err != nil {
				t.Fatal(err)
			}
			project, err := lib.GetProject(ctx, "project")
			if err != nil {
				t.Fatal(err)
			}
			for attempt := 0; attempt < 2; attempt++ {
				release, err := lib.GetRelease(ctx, "current")
				if err != nil {
					t.Fatal(err)
				}
				target := checkTargetFrom(project, release)
				result, err := service.checkGitHub(ctx, target, func(string) {})
				if err != nil {
					t.Fatal(err)
				}
				if result.UpdateAvailable != tc.wantUpdate || result.DetectedVersion != "2.0.0" {
					t.Fatalf("attempt %d: result %+v, want update %v", attempt, result, tc.wantUpdate)
				}
				if err := service.persistObservation(ctx, target, result); err != nil {
					t.Fatal(err)
				}
			}
			wantDownloads := 0
			if tc.wantUpdate {
				wantDownloads = 2
			}
			if downloads != wantDownloads {
				t.Fatalf("downloads = %d, want %d", downloads, wantDownloads)
			}
		})
	}
}

func TestTrackingFollowsStableSuccessorOfNormalizedPrerelease(t *testing.T) {
	preview := library.Release{ID: "preview", VendorVersion: "0.1.0_rc.4", SourceType: "arch-package",
		State: "built", Document: map[string]any{"acquisition": map[string]any{
			"kind": "github-release", "githubPrerelease": true,
		}}}
	stable := library.Release{ID: "stable", VendorVersion: "0.1.0", SourceType: "arch-package",
		State: "ready", Document: map[string]any{"acquisition": map[string]any{
			"kind": "github-release", "githubPrerelease": false,
		}}}
	for _, releases := range [][]library.Release{{preview, stable}, {stable, preview}} {
		selected := activeTrackingRelease(library.Project{Releases: releases})
		if selected == nil || selected.ID != stable.ID {
			t.Fatalf("tracking release = %+v, want stable successor", selected)
		}
	}
	if releaseVersionCompare(stable, preview) <= 0 || releaseVersionCompare(preview, stable) >= 0 {
		t.Fatal("stable successor must sort after its prerelease for automatic build reconciliation")
	}
	preview.VendorVersion = "0.2.0_rc.1"
	if releaseVersionCompare(stable, preview) >= 0 {
		t.Fatal("stable release must not sort after a prerelease of a newer version")
	}
}
