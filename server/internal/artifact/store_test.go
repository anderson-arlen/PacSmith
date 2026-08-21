package artifact

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

func TestIngestIsContentAddressedAndAtomic(t *testing.T) {
	root := t.TempDir()
	store, err := New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	payload := []byte("hello pacsmith")
	first, err := store.Ingest(bytes.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	second, err := store.Ingest(bytes.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	if first.SHA256 != second.SHA256 || first.Size != int64(len(payload)) {
		t.Fatalf("objects %+v %+v", first, second)
	}
	path, err := store.Path(first.SHA256)
	if err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, payload) {
		t.Fatal("object bytes mismatch")
	}
}

func TestOrphanTempIsNotReferenced(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	objects := filepath.Join(root, "objects")
	tmp := filepath.Join(root, "tmp")
	store, err := New(objects, tmp)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(tmp, "ingest-crash.part"), []byte("partial"), 0o600); err != nil {
		t.Fatal(err)
	}
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	registry := &Registry{DB: db, Store: store}
	_, err = registry.Get(ctx, "00000000-0000-0000-0000-000000000001")
	if err != ErrNotFound {
		t.Fatalf("expected not found, got %v", err)
	}
	entries, err := os.ReadDir(objects)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 0 {
		t.Fatalf("objects dir should be empty, got %v", entries)
	}
}

func TestSanitizeFilename(t *testing.T) {
	got, err := SanitizeFilename("/tmp/../evil.deb")
	if err != nil {
		t.Fatal(err)
	}
	if got != "evil.deb" {
		t.Fatalf("got %q", got)
	}
	if _, err := SanitizeFilename(".."); err == nil {
		t.Fatal("expected error")
	}
}
