package legacy

import "testing"

func TestContainsProjects(t *testing.T) {
	cases := map[string]bool{
		"/home/user/.local/share/pacsmith/projects":          true,
		"/home/user/.local/share/pacsmith/projects/app":      true,
		"/home/user/.local/share/pacsmith/projects/app/json": true,
		"/home/user/.local/share/pacsmith/server":            false,
		"/home/user/.local/share/pacsmith/server/objects":    false,
		"/home/user/.local/share/pacsmith/client":            false,
		"/tmp/pacsmith/projects":                             true,
		"/tmp/not-pacsmith/projects":                         false,
		"/tmp/pacsmith-projects":                             false,
		"/tmp/pacsmith/server":                               false,
	}
	for path, want := range cases {
		if got := ContainsProjects(path); got != want {
			t.Errorf("ContainsProjects(%q)=%v want %v", path, got, want)
		}
	}
}

func TestForbid(t *testing.T) {
	if err := Forbid("/var/lib/pacsmith/server", "data"); err != nil {
		t.Fatal(err)
	}
	if err := Forbid("/var/lib/pacsmith/projects", "data"); err == nil {
		t.Fatal("expected forbid")
	}
}
