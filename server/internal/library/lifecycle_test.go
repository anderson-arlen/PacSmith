package library

import (
	"os"
	"path/filepath"
	"testing"
)

func TestOriginalArchLifecycleReuse(t *testing.T) {
	marker := filepath.Join(t.TempDir(), "executed")
	contents := "_reload() { local user; user=$(printf root); }\npost_install() { _reload; }\nprintf executed > '" + marker + "'\n"
	for _, test := range []struct {
		name, sourceType, scriptName, original, candidate string
		passed                                            bool
	}{
		{"original", "arch-package", ".INSTALL", contents, contents, true},
		{"edited", "arch-package", ".INSTALL", contents, contents + "\n", false},
		{"debian", "deb", ".INSTALL", contents, contents, false},
		{"other-script", "arch-package", "postinst", contents, contents, false},
		{"missing", "arch-package", ".INSTALL", "", contents, false},
		{"syntax", "arch-package", ".INSTALL", "post_install() { if; }", "post_install() { if; }", false},
		{"no-hook", "arch-package", ".INSTALL", "helper() { :; }", "helper() { :; }", false},
	} {
		t.Run(test.name, func(t *testing.T) {
			document := map[string]any{
				"sourceType":        test.sourceType,
				"maintainerScripts": []any{map[string]any{"name": test.scriptName, "contents": test.original}},
			}
			validation := validateReleaseLifecycle(document, test.candidate)
			if validation.Passed != test.passed {
				t.Fatalf("passed = %v: %s", validation.Passed, validation.Message())
			}
		})
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatalf("syntax validation executed the script: %v", err)
	}
}
