package paths

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/anderson-arlen/pacsmith/server/internal/legacy"
)

const (
	appDir     = legacy.AppDirName
	serverDir  = legacy.ServerDirName
	socketName = "pacsmith.sock"
	dbName     = "pacsmith.db"
)

// Dirs is the complete server filesystem layout. Every field is an absolute
// path after Resolve.
type Dirs struct {
	DataHome string
	Data     string
	Config   string
	State    string
	Runtime  string
	Socket   string
	Database string
	Objects  string
	Work     string
	Tmp      string
}

// Overrides replace individual XDG roots. Empty fields fall back to the
// process environment / specification defaults.
type Overrides struct {
	DataHome   string
	ConfigHome string
	StateHome  string
	RuntimeDir string
	Socket     string
}

func Resolve(over Overrides) (Dirs, error) {
	dataHome, err := xdgDir(over.DataHome, "XDG_DATA_HOME", ".local/share")
	if err != nil {
		return Dirs{}, err
	}
	configHome, err := xdgDir(over.ConfigHome, "XDG_CONFIG_HOME", ".config")
	if err != nil {
		return Dirs{}, err
	}
	stateHome, err := xdgDir(over.StateHome, "XDG_STATE_HOME", ".local/state")
	if err != nil {
		return Dirs{}, err
	}
	runtimeDir, err := runtimeRoot(over.RuntimeDir)
	if err != nil {
		return Dirs{}, err
	}

	dirs := Dirs{
		DataHome: dataHome,
		Data:     filepath.Join(dataHome, appDir, serverDir),
		Config:   filepath.Join(configHome, appDir, serverDir),
		State:    filepath.Join(stateHome, appDir, serverDir),
		Runtime:  filepath.Join(runtimeDir, appDir),
	}
	dirs.Database = filepath.Join(dirs.Data, dbName)
	dirs.Objects = filepath.Join(dirs.Data, "objects")
	dirs.Work = filepath.Join(dirs.Data, "work")
	dirs.Tmp = filepath.Join(dirs.Data, "tmp")
	if over.Socket != "" {
		dirs.Socket = over.Socket
	} else {
		dirs.Socket = filepath.Join(dirs.Runtime, socketName)
	}
	if err := validate(dirs); err != nil {
		return Dirs{}, err
	}
	return dirs, nil
}

func (d Dirs) Ensure() error {
	if err := validate(d); err != nil {
		return err
	}
	for _, dir := range []string{d.Data, d.Config, d.State, d.Runtime, d.Objects, d.Work, d.Tmp} {
		if err := os.MkdirAll(dir, 0o700); err != nil {
			return fmt.Errorf("create %s: %w", dir, err)
		}
		if err := os.Chmod(dir, 0o700); err != nil {
			return fmt.Errorf("chmod %s: %w", dir, err)
		}
	}
	return nil
}

func validate(d Dirs) error {
	return legacy.ForbidAll(map[string]string{
		"data":     d.Data,
		"config":   d.Config,
		"state":    d.State,
		"runtime":  d.Runtime,
		"socket":   d.Socket,
		"database": d.Database,
		"objects":  d.Objects,
		"work":     d.Work,
		"tmp":      d.Tmp,
	})
}

func xdgDir(override, env, relativeToHome string) (string, error) {
	if override != "" {
		return absDir(override)
	}
	if value := os.Getenv(env); value != "" {
		return absDir(value)
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("home directory: %w", err)
	}
	return absDir(filepath.Join(home, relativeToHome))
}

func runtimeRoot(override string) (string, error) {
	if override != "" {
		return absDir(override)
	}
	if value := os.Getenv("XDG_RUNTIME_DIR"); value != "" {
		return absDir(value)
	}
	return absDir(filepath.Join("/run/user", fmt.Sprintf("%d", os.Getuid())))
}

func absDir(path string) (string, error) {
	if path == "" {
		return "", fmt.Errorf("empty directory path")
	}
	if !filepath.IsAbs(path) {
		return "", fmt.Errorf("directory %q is not absolute", path)
	}
	return filepath.Clean(path), nil
}
