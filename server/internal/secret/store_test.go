package secret

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/godbus/dbus/v5"
)

func TestFileStorePermissions(t *testing.T) {
	ctx := context.Background()
	dir := filepath.Join(t.TempDir(), "secrets")
	store, err := NewFileStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(dir)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o700 {
		t.Fatalf("directory mode %o, want 0700", info.Mode().Perm())
	}
	if err := store.Set(ctx, "github.token", []byte("s3cret")); err != nil {
		t.Fatal(err)
	}
	fileInfo, err := os.Stat(filepath.Join(dir, "github.token"))
	if err != nil {
		t.Fatal(err)
	}
	if fileInfo.Mode().Perm() != 0o600 {
		t.Fatalf("file mode %o, want 0600", fileInfo.Mode().Perm())
	}
	got, err := store.Get(ctx, "github.token")
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "s3cret" {
		t.Fatalf("got %q", got)
	}
}

func TestFileStoreRejectsPathTraversal(t *testing.T) {
	ctx := context.Background()
	store, err := NewFileStore(filepath.Join(t.TempDir(), "secrets"))
	if err != nil {
		t.Fatal(err)
	}
	names := []string{
		"../secret",
		"..",
		".",
		"foo/bar",
		"/etc/passwd",
		"foo/../../etc/passwd",
		"HasUpper",
		"has space",
		"",
		"foo\x00bar",
	}
	for _, name := range names {
		if err := store.Set(ctx, name, []byte("x")); !errors.Is(err, ErrInvalidName) {
			t.Errorf("Set(%q) error %v, want ErrInvalidName", name, err)
		}
		if _, err := store.Get(ctx, name); !errors.Is(err, ErrInvalidName) {
			t.Errorf("Get(%q) error %v, want ErrInvalidName", name, err)
		}
	}
}

func TestEnvStoreGetOnly(t *testing.T) {
	ctx := context.Background()
	t.Setenv("PACSMITH_SECRET_GITHUB_TOKEN", "from-env")
	store := NewEnvStore()
	got, err := store.Get(ctx, "github.token")
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "from-env" {
		t.Fatalf("got %q", got)
	}
	if err := store.Set(ctx, "github.token", []byte("nope")); !errors.Is(err, ErrReadOnly) {
		t.Fatalf("Set error %v, want ErrReadOnly", err)
	}
	again, err := store.Get(ctx, "github.token")
	if err != nil {
		t.Fatal(err)
	}
	if string(again) != "from-env" {
		t.Fatal("Set mutated environment-backed secret")
	}
	if err := store.Delete(ctx, "github.token"); !errors.Is(err, ErrReadOnly) {
		t.Fatalf("Delete error %v, want ErrReadOnly", err)
	}
}

func TestLockedStoreDoesNotSwitchBackends(t *testing.T) {
	ctx := context.Background()
	fileDir := filepath.Join(t.TempDir(), "secrets")
	files, err := NewFileStore(fileDir)
	if err != nil {
		t.Fatal(err)
	}
	inner := stubStore{err: ErrUnavailable}
	locked := NewLockedStore(BackendSecretService, inner)
	if locked.Backend() != BackendSecretService {
		t.Fatalf("backend %q", locked.Backend())
	}
	if err := locked.Set(ctx, "ca.key", []byte("secret")); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("Set error %v, want ErrUnavailable", err)
	}
	if _, err := locked.Get(ctx, "ca.key"); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("Get error %v, want ErrUnavailable", err)
	}
	if locked.Backend() != BackendSecretService {
		t.Fatal("LockedStore switched backends after a failure")
	}
	exists, err := files.Exists(ctx, "ca.key")
	if err != nil {
		t.Fatal(err)
	}
	if exists {
		t.Fatal("LockedStore fell back to the file backend")
	}
}

func TestSecretServiceStoreUnavailableWithoutSessionBus(t *testing.T) {
	ctx := context.Background()
	store := &SecretServiceStore{connect: func() (*dbus.Conn, error) {
		return nil, errors.New("no session bus")
	}}
	if _, err := store.Get(ctx, "ca.key"); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("Get error %v, want ErrUnavailable", err)
	}
	if err := store.Set(ctx, "ca.key", []byte("x")); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("Set error %v, want ErrUnavailable", err)
	}
}

func TestSecretServiceStoreRoundTripIfAvailable(t *testing.T) {
	ctx := context.Background()
	store := NewSecretServiceStore()
	name := "pacsmith-test-daemon-secret"
	t.Cleanup(func() { _ = store.Delete(ctx, name) })
	if err := store.Set(ctx, name, []byte("hello")); err != nil {
		if errors.Is(err, ErrUnavailable) {
			return
		}
		t.Fatal(err)
	}
	got, err := store.Get(ctx, name)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "hello" {
		t.Fatalf("got %q", got)
	}
}

type stubStore struct {
	err error
}

func (s stubStore) Get(context.Context, string) ([]byte, error) { return nil, s.err }
func (s stubStore) Set(context.Context, string, []byte) error   { return s.err }
func (s stubStore) Delete(context.Context, string) error        { return s.err }
func (s stubStore) Exists(context.Context, string) (bool, error) {
	return false, s.err
}
