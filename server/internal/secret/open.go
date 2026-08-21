package secret

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

const BackendEnvName = "PACSMITH_SECRET_BACKEND"

type Opened struct {
	Store   *LockedStore
	Backend string
}

// Open pins the persisted SecretStore backend. First init selects Secret
// Service when a usable session exists, otherwise the 0700 file store.
// Later startups fail if that backend is gone; there is no silent downgrade.
func Open(ctx context.Context, db *sqlite.DB, secretsDir string) (*Opened, error) {
	state, err := db.Queries.GetServerState(ctx)
	if err != nil {
		return nil, fmt.Errorf("server state: %w", err)
	}
	wanted := strings.TrimSpace(state.SecretBackend)
	if override := strings.TrimSpace(os.Getenv(BackendEnvName)); override != "" && wanted == "" {
		wanted = override
	}
	if wanted == "" {
		wanted = chooseBackend(ctx)
	}
	store, err := openBackend(secretsDir, wanted)
	if err != nil {
		return nil, err
	}
	if _, err := store.Exists(ctx, "pacsmith.init"); err != nil && !errors.Is(err, ErrNotFound) {
		return nil, fmt.Errorf("secret backend %s: %w", wanted, err)
	}
	if state.SecretBackend != wanted {
		if err := db.Queries.UpdateServerBackend(ctx, sqlcdb.UpdateServerBackendParams{
			SecretBackend: wanted,
			PkiReady:      state.PkiReady,
		}); err != nil {
			return nil, err
		}
	}
	return &Opened{Store: NewLockedStore(wanted, store), Backend: wanted}, nil
}

func chooseBackend(ctx context.Context) string {
	ss := NewSecretServiceStore()
	_, err := ss.Exists(ctx, "pacsmith.init")
	if err == nil || errors.Is(err, ErrNotFound) {
		return BackendSecretService
	}
	return BackendFile
}

func openBackend(secretsDir, backend string) (Store, error) {
	switch backend {
	case BackendFile:
		return NewFileStore(filepath.Join(secretsDir, "secrets"))
	case BackendEnv:
		return NewEnvStore(), nil
	case BackendSecretService:
		return NewSecretServiceStore(), nil
	default:
		return nil, fmt.Errorf("unknown secret backend %q", backend)
	}
}

func IsInternalName(name string) bool {
	return strings.HasPrefix(name, "pki.")
}
