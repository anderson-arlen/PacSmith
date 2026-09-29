package library

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
	"github.com/anderson-arlen/pacsmith/server/internal/recipe"
)

func TestSystemIconSurvivesDocumentSummaryAndUpdate(t *testing.T) {
	analysis := inspect.Analysis{
		Type: inspect.SourceELF,
		Install: inspect.InstallMapping{
			Icon: inspect.IconConfiguration{SourceKind: inspect.IconSystemTheme, IconName: "utilities-terminal"},
			DesktopEntries: []inspect.DesktopEntry{{
				Enabled:     true,
				Destination: "/usr/share/applications/vendor-tool.desktop",
				Contents:    "[Desktop Entry]\nType=Application\nName=Vendor tool\nExec=vendor-tool\nIcon=utilities-terminal\n",
			}},
		},
	}
	alignIntegrationIconName(&analysis, "vendor-tool-bin")
	raw, err := analysisDocument("vendor-tool", strings.Repeat("a", 64), "", analysis)
	if err != nil {
		t.Fatal(err)
	}
	var document map[string]any
	if err := json.Unmarshal([]byte(raw), &document); err != nil {
		t.Fatal(err)
	}
	if !releaseIconConfigured(document) {
		t.Fatalf("system icon not configured: %+v", document)
	}
	install, _ := mapValue(document, "installMapping")
	icon, _ := mapValue(install, "icon")
	if stringValue(icon, "sourceKind") != "system-theme" || stringValue(icon, "iconName") != "utilities-terminal" {
		t.Fatalf("system icon changed during serialization: %+v", icon)
	}
	for _, artifactID := range []string{"", "previous-image-artifact"} {
		summary := Release{Document: map[string]any{}}
		attachReleaseIconSummary(&summary, raw, artifactID)
		if !releaseIconConfigured(summary.Document) || summary.Document["iconArtifactId"] != "" {
			t.Fatalf("summary requires image artifact: %+v", summary.Document)
		}
	}
	nextInstall := map[string]any{}
	carryIcon(install, nextInstall, map[string]any{})
	carried, _ := mapValue(nextInstall, "icon")
	if stringValue(carried, "iconName") != "utilities-terminal" || boolValue(carried, "missing") {
		t.Fatalf("system icon lost during update: %+v", carried)
	}
	rel := recipeFromDocument(Release{Document: document})
	if recipe.IconSourceName(rel) != "" {
		t.Fatal("system icon requires a bundled image")
	}
	generated := recipe.Generate(rel)
	if !strings.Contains(generated, "Icon=utilities-terminal") {
		t.Fatalf("system icon missing from desktop launcher: %s", generated)
	}
	if strings.Contains(generated, "pacsmith-icon.") || strings.Contains(generated, "Install the selected, content-addressed application icon") {
		t.Fatalf("system icon generated an image installation: %s", generated)
	}
}
