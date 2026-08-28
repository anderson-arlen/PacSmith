package library

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"path/filepath"
	"strings"
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

func TestManualImportOverridesVersionAndVerifiesPublisherSHA256(t *testing.T) {
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
	first, err := registry.Put(ctx, "android-studio-previous-linux", "source", bytes.NewReader(elf))
	if err != nil {
		t.Fatal(err)
	}
	created, err := svc.ImportArtifact(ctx, ImportRequest{
		ArtifactID: first.ID,
		Version:    "2026.1.3.7",
	})
	if err != nil {
		t.Fatal(err)
	}

	nextBytes := append(append([]byte{}, elf...), 0)
	next, err := registry.Put(ctx, "android-studio-quail3-patch1-linux", "source", bytes.NewReader(nextBytes))
	if err != nil {
		t.Fatal(err)
	}
	_, err = svc.ImportArtifact(ctx, ImportRequest{
		ArtifactID:        next.ID,
		ExistingProjectID: created.ProjectID,
		Version:           "2026.1.3.8",
		ExpectedSHA256:    strings.Repeat("0", 64),
	})
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "publisher SHA256 mismatch") {
		t.Fatalf("checksum mismatch error = %v", err)
	}

	imported, err := svc.ImportArtifact(ctx, ImportRequest{
		ArtifactID:        next.ID,
		ExistingProjectID: created.ProjectID,
		Version:           "2026.1.3.8",
		ExpectedSHA256:    strings.ToUpper(next.SHA256),
	})
	if err != nil {
		t.Fatal(err)
	}
	release, err := db.Queries.GetRelease(ctx, imported.ReleaseID)
	if err != nil {
		t.Fatal(err)
	}
	if release.VendorVersion != "2026.1.3.8" {
		t.Fatalf("version = %q", release.VendorVersion)
	}
	var document map[string]any
	if err := json.Unmarshal([]byte(release.BodyJson), &document); err != nil {
		t.Fatal(err)
	}
	acquisition, _ := document["acquisition"].(map[string]any)
	if acquisition["publisherDigest"] != next.SHA256 || acquisition["publisherVerified"] != true {
		t.Fatalf("acquisition = %#v", acquisition)
	}
	identityVariables, _ := document["identityVariables"].(string)
	if !strings.Contains(identityVariables, "_PACSMITH_PKGVER='2026.1.3.8'") {
		t.Fatalf("identity variables = %q", identityVariables)
	}
}
