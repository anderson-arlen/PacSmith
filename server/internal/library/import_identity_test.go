package library

import (
	"bytes"
	"context"
	"encoding/binary"
	"path/filepath"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

func TestImportArtifactUsesOriginalFilenameForRawIdentity(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	store, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	registry := &artifact.Registry{DB: db, Store: store}
	svc := &Service{DB: db, Artifacts: registry, WorkDir: filepath.Join(root, "work")}

	elf := make([]byte, 64)
	copy(elf, []byte{0x7f, 'E', 'L', 'F'})
	elf[4] = 2
	elf[5] = 1
	binary.LittleEndian.PutUint16(elf[18:20], 62)
	record, err := registry.Put(ctx, "chamber-v3.1.5-linux-amd64", "source", bytes.NewReader(elf))
	if err != nil {
		t.Fatal(err)
	}

	result, err := svc.ImportArtifact(ctx, ImportRequest{
		ArtifactID:        record.ID,
		AcquisitionKind:   "github-release",
		CanonicalIdentity: "github:segmentio/chamber",
	})
	if err != nil {
		t.Fatal(err)
	}
	project, err := db.Queries.GetProject(ctx, result.ProjectID)
	if err != nil {
		t.Fatal(err)
	}
	if project.DisplayName != "chamber" || project.ArchPackageName != "chamber-bin" {
		t.Fatalf("project identity: display=%q package=%q", project.DisplayName, project.ArchPackageName)
	}
	release, err := db.Queries.GetRelease(ctx, result.ReleaseID)
	if err != nil {
		t.Fatal(err)
	}
	if release.VendorVersion != "3.1.5" || release.OriginalFilename != "chamber-v3.1.5-linux-amd64" {
		t.Fatalf("release identity: version=%q filename=%q", release.VendorVersion, release.OriginalFilename)
	}
}
