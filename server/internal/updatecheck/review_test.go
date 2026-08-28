package updatecheck

import (
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/library"
)

func TestAutomaticReviewBlockersRejectChangedDependencySurface(t *testing.T) {
	previous := reviewFixture("libold (>= 1)")
	next := reviewFixture("libnew (>= 1)")
	blockers := automaticReviewBlockers(previous, next)
	if !containsBlocker(blockers, "vendor dependency declarations changed") {
		t.Fatalf("blockers = %v", blockers)
	}
}

func TestAutomaticReviewBlockersAllowIdenticalReviewedSurface(t *testing.T) {
	previous := reviewFixture("libsame (>= 1)")
	next := reviewFixture("libsame (>= 1)")
	if blockers := automaticReviewBlockers(previous, next); len(blockers) != 0 {
		t.Fatalf("blockers = %v", blockers)
	}
}

func TestReleaseReviewIssuesRecognizePersistedUnresolvedDependency(t *testing.T) {
	document := reviewFixture("libmissing (>= 1)")
	document["dependencies"] = []any{map[string]any{
		"rawExpression": "libmissing (>= 1)", "status": "Unresolved",
	}}
	issues := releaseReviewIssues(document)
	if len(issues) != 1 || issues[0] != "vendor dependency has no reviewed Arch treatment" {
		t.Fatalf("issues = %v", issues)
	}
}

func TestNormalizeAPTFilePathAllowsFlatRepositoryFilesOnly(t *testing.T) {
	if got := normalizeAPTFilePath("./typora_1.14.9_amd64.deb"); got != "typora_1.14.9_amd64.deb" {
		t.Fatalf("normalized flat path = %q", got)
	}
	if got := normalizeAPTFilePath("./../escape.deb"); safeRepositoryPath(got, false) {
		t.Fatalf("normalized traversal path %q was accepted", got)
	}
}

func TestReleaseReviewUsesExactLifecycleContentsForAcknowledgement(t *testing.T) {
	document := reviewFixture("libsame (>= 1)")
	contents := "post_install() {\n  update-desktop-database -q\n}\n"
	document["lifecycleScript"] = map[string]any{
		"fileName": "demo.install", "contents": contents, "validationPassed": true,
		"acknowledgedFingerprint": namedContentFingerprint("demo.install", contents),
		"sourceFingerprints":      []any{},
	}
	if issues := releaseReviewIssues(document); len(issues) != 0 {
		t.Fatalf("exact acknowledged lifecycle contents produced issues: %v", issues)
	}
}

func TestAutomaticReviewBlockersDoNotTreatLifecycleProvenanceAsBehavior(t *testing.T) {
	previous := reviewFixture("libsame (>= 1)")
	next := reviewFixture("libsame (>= 1)")
	contents := "post_install() {\n  update-desktop-database -q\n}\n"
	for _, item := range []struct {
		document map[string]any
		source   string
	}{{previous, "previous-evidence"}, {next, "next-evidence"}} {
		item.document["lifecycleScript"] = map[string]any{
			"fileName": "demo.install", "contents": contents, "validationPassed": true,
			"acknowledgedFingerprint": namedContentFingerprint("demo.install", contents),
			"sourceFingerprints":      []any{item.source},
		}
	}
	if blockers := automaticReviewBlockers(previous, next); len(blockers) != 0 {
		t.Fatalf("provenance-only lifecycle change produced blockers: %v", blockers)
	}
}

func TestReleaseHasSuccessfulBuildRecognizesStatusAndArtifacts(t *testing.T) {
	for _, document := range []map[string]any{
		{"buildStatus": "succeeded"},
		{"builtArtifactIds": []any{"package"}},
	} {
		if !releaseHasSuccessfulBuild(library.Release{Document: document}) {
			t.Fatalf("successful release was not recognized: %#v", document)
		}
	}
	if releaseHasSuccessfulBuild(library.Release{Document: map[string]any{
		"buildStatus": "never-built", "builtArtifactIds": []any{},
	}}) {
		t.Fatal("unbuilt release was recognized as successfully built")
	}
}

func reviewFixture(dependency string) map[string]any {
	return map[string]any{
		"buildStatus": "succeeded", "builtArtifactIds": []any{"artifact"},
		"sourceType": "deb", "dependencies": []any{map[string]any{
			"rawExpression": dependency, "status": "mapped",
		}},
		"maintainerScripts": []any{}, "scriptFindings": []any{},
		"payload": []any{}, "payloadRules": []any{},
		"installMapping": map[string]any{
			"launchers": []any{}, "desktopEntries": []any{},
			"icon":   map[string]any{"missing": false},
			"appRun": map[string]any{"present": false},
		},
	}
}

func containsBlocker(blockers []string, wanted string) bool {
	for _, blocker := range blockers {
		if blocker == wanted {
			return true
		}
	}
	return false
}
