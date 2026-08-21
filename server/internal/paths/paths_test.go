package paths

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestResolveUsesServerXDGLayout(t *testing.T) {
	root := t.TempDir()
	dirs, err := Resolve(Overrides{
		DataHome:   filepath.Join(root, "data"),
		ConfigHome: filepath.Join(root, "config"),
		StateHome:  filepath.Join(root, "state"),
		RuntimeDir: filepath.Join(root, "runtime"),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := dirs.Ensure(); err != nil {
		t.Fatal(err)
	}
	wantSuffix := func(path, suffix string) {
		t.Helper()
		if !strings.HasSuffix(path, suffix) {
			t.Fatalf("%s does not end with %s", path, suffix)
		}
	}
	wantSuffix(dirs.Data, "/pacsmith/server")
	wantSuffix(dirs.Config, "/pacsmith/server")
	wantSuffix(dirs.State, "/pacsmith/server")
	wantSuffix(dirs.Runtime, "/pacsmith")
	wantSuffix(dirs.Socket, "/pacsmith/pacsmith.sock")
	if _, err := os.Stat(filepath.Join(root, "data", "pacsmith", "projects")); !os.IsNotExist(err) {
		t.Fatalf("Ensure created legacy projects dir: %v", err)
	}
}

func TestResolveRejectsLegacyProjectsPath(t *testing.T) {
	root := t.TempDir()
	_, err := Resolve(Overrides{
		DataHome:   filepath.Join(root, "data"),
		ConfigHome: filepath.Join(root, "config"),
		StateHome:  filepath.Join(root, "state"),
		RuntimeDir: filepath.Join(root, "runtime"),
		Socket:     filepath.Join(root, "data", "pacsmith", "projects", "pacsmith.sock"),
	})
	if err == nil {
		t.Fatal("expected legacy path rejection")
	}
}

func TestResolveRequiresAbsolutePaths(t *testing.T) {
	_, err := Resolve(Overrides{DataHome: "relative"})
	if err == nil {
		t.Fatal("expected error")
	}
}
