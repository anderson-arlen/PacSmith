package apitest

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func AssembleSampleDeb(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("caller")
	}
	fixture := filepath.Join(filepath.Dir(file), "..", "..", "..", "client", "tests", "fixtures", "sample-deb")
	bsdtar, err := exec.LookPath("bsdtar")
	if err != nil {
		t.Fatal(err)
	}
	ar, err := exec.LookPath("ar")
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	packageDir := filepath.Join(dir, "package")
	if err := os.MkdirAll(packageDir, 0o755); err != nil {
		t.Fatal(err)
	}
	debianBinary, err := os.ReadFile(filepath.Join(fixture, "debian-binary"))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(packageDir, "debian-binary"), debianBinary, 0o644); err != nil {
		t.Fatal(err)
	}
	run := func(name string, args ...string) {
		t.Helper()
		cmd := exec.Command(name, args...)
		cmd.Dir = packageDir
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("%s %s: %v\n%s", name, strings.Join(args, " "), err, out)
		}
	}
	run(bsdtar, "-caf", "control.tar.zst", "-C", filepath.Join(fixture, "control"), ".")
	run(bsdtar, "-caf", "data.tar.zst", "-C", filepath.Join(fixture, "data"), ".")
	run(ar, "r", "sample.deb", "debian-binary", "control.tar.zst", "data.tar.zst")
	return filepath.Join(packageDir, "sample.deb")
}
