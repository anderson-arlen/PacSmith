package repo

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

func TestRecoverMissingSigningKeyDisablesAndResetsRepository(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	objects, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	fileStore, err := secret.NewFileStore(filepath.Join(root, "secrets"))
	if err != nil {
		t.Fatal(err)
	}
	gnupg := filepath.Join(root, "gnupg")
	if err := os.MkdirAll(gnupg, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(gnupg, "stale-private-key"), []byte("stale"), 0o600); err != nil {
		t.Fatal(err)
	}
	svc := New(db, &artifact.Registry{DB: db, Store: objects},
		secret.NewLockedStore(secret.BackendFile, fileStore), filepath.Join(root, "work"), gnupg)

	row, err := db.Queries.GetRepoSettings(ctx)
	if err != nil {
		t.Fatal(err)
	}
	row.Enabled = 1
	row.SigningInitialized = 1
	row.SigningFingerprint = strings.Repeat("A", 40)
	row.ModifiedAt = "2026-01-01T00:00:00Z"
	if _, err := db.Queries.UpdateRepoSettings(ctx, updateParamsFrom(row, row.Revision)); err != nil {
		t.Fatal(err)
	}

	changed, err := svc.RecoverMissingSigningKey(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if !changed {
		t.Fatal("missing signing key was not recovered")
	}
	settings, err := svc.Settings(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if settings.Enabled || settings.SigningInitialized || settings.Fingerprint != "" {
		t.Fatalf("repository was not disabled and reset: %+v", settings)
	}
	if !strings.Contains(settings.RecoveryMessage, "private signing key was missing") {
		t.Fatalf("recovery message %q does not explain the failure", settings.RecoveryMessage)
	}
	if _, err := os.Stat(gnupg); !os.IsNotExist(err) {
		t.Fatalf("stale GnuPG home still exists: %v", err)
	}
	enabled := true
	if _, err := svc.PatchSettings(ctx, SettingsPatch{Enabled: &enabled}); err == nil {
		t.Fatal("repository was enabled without a replacement signing key")
	}
}
