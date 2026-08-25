package library

import "testing"

func TestCustomRecipeCopiesForwardVerbatim(t *testing.T) {
	previous := map[string]any{
		"pkgbuildManuallyModified": true,
		"customPkgbuild":           "source ./pacsmith.vars\npackage() { printf '%s' \"$pkgver\"; }\n",
		"customFiles": map[string]any{
			"wrapper.sh": "#!/bin/sh\nexec /opt/demo/demo \"$@\"\n",
			"fix.patch":  "--- old\n+++ new\n",
		},
		"sourceArtifactId": "old-source-object",
		"builds":           []any{"old-build"},
		"payload":          []any{"old-inspection"},
		"packageMetadata": map[string]any{
			"description":            "Demo application",
			"licenses":               []any{"MIT"},
			"additionalDependencies": []any{"libnotify"},
		},
	}
	next := map[string]any{
		"generatedPkgbuild":        "generated for 1.2.4",
		"pkgbuildManuallyModified": false,
		"identityVariables":        "new vars",
		"sourceArtifactId":         "new-source-object",
		"builds":                   []any{},
		"payload":                  []any{"new-inspection"},
	}

	carryForwardRelease(previous, next)

	if !boolValue(next, "pkgbuildManuallyModified") {
		t.Fatal("custom mode was not copied forward")
	}
	if got := stringValue(next, "customPkgbuild"); got != previous["customPkgbuild"] {
		t.Fatalf("PKGBUILD changed during copy-forward:\n%s", got)
	}
	files, ok := mapValue(next, "customFiles")
	if !ok || files["wrapper.sh"] != previous["customFiles"].(map[string]any)["wrapper.sh"] ||
		files["fix.patch"] != previous["customFiles"].(map[string]any)["fix.patch"] {
		t.Fatalf("custom support files were not copied verbatim: %#v", files)
	}
	if stringValue(next, "identityVariables") != "new vars" {
		t.Fatal("PacSmith-owned identity variables were overwritten")
	}
	if stringValue(next, "sourceArtifactId") != "new-source-object" ||
		len(next["builds"].([]any)) != 0 || next["payload"].([]any)[0] != "new-inspection" {
		t.Fatal("PacSmith-owned release state was copied from the historical release")
	}
	if _, exists := next["previousManualPkgbuild"]; exists {
		t.Fatal("obsolete previousManualPkgbuild was retained")
	}
	metadata, ok := mapValue(next, "packageMetadata")
	if !ok || stringValue(metadata, "description") != "Demo application" ||
		len(stringSlice(metadata["additionalDependencies"])) != 1 {
		t.Fatalf("ordinary package metadata was not carried forward: %#v", metadata)
	}
	metadata["description"] = "Changed only in 1.2.4"
	previousMetadata := previous["packageMetadata"].(map[string]any)
	if previousMetadata["description"] == metadata["description"] {
		t.Fatal("new release package metadata aliases the historical release")
	}

	files["wrapper.sh"] = "changed only in 1.2.4"
	if previous["customFiles"].(map[string]any)["wrapper.sh"] == files["wrapper.sh"] {
		t.Fatal("new release support files alias the historical release")
	}
	next["customPkgbuild"] = "changed only in 1.2.4"
	if previous["customPkgbuild"] == next["customPkgbuild"] {
		t.Fatal("new release PKGBUILD edit changed the historical release")
	}
}

func TestGuidedReleaseDoesNotInheritCustomFiles(t *testing.T) {
	previous := map[string]any{
		"pkgbuildManuallyModified": false,
		"customFiles":              map[string]any{"stale.patch": "must not copy"},
	}
	next := map[string]any{"identityVariables": "release-specific"}
	carryForwardRelease(previous, next)
	if _, exists := next["customFiles"]; exists {
		t.Fatal("guided release inherited custom support files")
	}
}
