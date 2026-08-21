package legacy

import (
	"fmt"
	"path/filepath"
	"strings"
)

const (
	AppDirName      = "pacsmith"
	ProjectsDirName = "projects"
	ServerDirName   = "server"
	ClientDirName   = "client"
)

// ProjectsDir is the pre-refactor library root. New code must never use it.
func ProjectsDir(xdgDataHome string) string {
	return filepath.Join(xdgDataHome, AppDirName, ProjectsDirName)
}

// ContainsProjects reports whether path is the legacy library root or anything
// inside it. Matching is by path components so a coincidental filename is not
// enough; the cleaned path must contain .../pacsmith/projects.
func ContainsProjects(path string) bool {
	cleaned := filepath.Clean(path)
	if cleaned == "" || cleaned == "." {
		return false
	}
	parts := splitPath(cleaned)
	for i := 0; i+1 < len(parts); i++ {
		if parts[i] == AppDirName && parts[i+1] == ProjectsDirName {
			return true
		}
	}
	return false
}

func splitPath(path string) []string {
	var parts []string
	for path != "" && path != string(filepath.Separator) && path != "." {
		base := filepath.Base(path)
		if base != "." && base != string(filepath.Separator) {
			parts = append([]string{base}, parts...)
		}
		parent := filepath.Dir(path)
		if parent == path {
			break
		}
		path = parent
	}
	return parts
}

func Forbid(path, what string) error {
	if ContainsProjects(path) {
		return fmt.Errorf("%s path %q is inside the legacy PacSmith library ($XDG_DATA_HOME/pacsmith/projects) and must not be used", what, path)
	}
	return nil
}

func ForbidAll(named map[string]string) error {
	var problems []string
	for what, path := range named {
		if err := Forbid(path, what); err != nil {
			problems = append(problems, err.Error())
		}
	}
	if len(problems) == 0 {
		return nil
	}
	return fmt.Errorf("%s", strings.Join(problems, "; "))
}
