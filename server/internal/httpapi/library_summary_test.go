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
