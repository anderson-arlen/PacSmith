package httpapi

import (
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
)

func TestEncodeProjectSummaryIncludesRepositoryDistribution(t *testing.T) {
	project := library.Project{
		ID:                   "project-1",
		DisplayName:          "Demo",
		ArchPackageName:      "demo-bin",
		RepoPublish:          true,
		RepoPkgnameOverride:  "demo-custom",
		RepoPublishedPkgname: "demo-published",
	}
	encoded := encodeProjectSummary(project, &repo.Settings{PackageNamePrefix: "vendor-"})
	if encoded["summaryOnly"] != true {
		t.Fatalf("project summary is not marked read-only: %+v", encoded)
	}
	repository, ok := encoded["repository"].(map[string]any)
	if !ok {
		t.Fatalf("repository summary has type %T", encoded["repository"])
	}
	if repository["publish"] != true ||
		repository["effective_package_name"] != "demo-custom" ||
		repository["published_package_name"] != "demo-published" {
		t.Fatalf("unexpected repository summary: %+v", repository)
	}
}

func TestEncodeProjectSummaryPoisonsReleaseRevisions(t *testing.T) {
	project := library.Project{
		ID:       "project-1",
		Releases: []library.Release{{ID: "release-1", Revision: 42}},
	}
	encoded := encodeProjectSummary(project, nil)
	releases, ok := encoded["releases"].([]map[string]any)
	if !ok || len(releases) != 1 || releases[0]["revision"] != int64(-1) {
		t.Fatalf("summary release revisions are writable: %+v", encoded["releases"])
	}
}
