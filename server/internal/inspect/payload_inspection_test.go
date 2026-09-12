package inspect

import (
	"archive/tar"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestInspectPayloadFileFromDebianPackage(t *testing.T) {
	deb := assembleSampleDeb(t)
	inspection, err := InspectPayloadFile(deb, "sample.deb", "usr/bin/pacsmith-smoke")
	if err != nil {
		t.Fatal(err)
	}
	want := []byte("#!/bin/sh\necho \"PacSmith fixture payload\"\n")
	sum := sha256.Sum256(want)
	if inspection.Path != "usr/bin/pacsmith-smoke" || inspection.Type != "file" ||
		inspection.Mode != "0755" || !inspection.Executable {
		t.Fatalf("metadata %+v", inspection)
	}
	if inspection.Size != int64(len(want)) || inspection.SHA256 != hex.EncodeToString(sum[:]) {
		t.Fatalf("content identity %+v", inspection)
	}
	if inspection.Text != string(want) || !strings.HasPrefix(inspection.MIME, "text/plain") {
		t.Fatalf("text inspection %+v", inspection)
	}
	if inspection.ELF != nil {
		t.Fatalf("shell script reported as ELF: %+v", inspection.ELF)
	}
}

func TestInspectPayloadFileELF(t *testing.T) {
	truePath, err := exec.LookPath("true")
	if err != nil {
		t.Skip("true not found")
	}
	inspection, err := InspectPayloadFile(truePath, filepath.Base(truePath), filepath.Base(truePath))
	if err != nil {
		t.Fatal(err)
	}
	contents, err := os.ReadFile(truePath)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(contents)
	if inspection.SHA256 != hex.EncodeToString(sum[:]) || inspection.MagicHex[:8] != "7f454c46" {
		t.Fatalf("ELF identity %+v", inspection)
	}
	if inspection.ELF == nil || inspection.ELF.Class == "" || inspection.ELF.Machine == "" ||
		len(inspection.ELF.ProgramHeaders) == 0 || len(inspection.ELF.Sections) == 0 {
		t.Fatalf("ELF inspection %+v", inspection.ELF)
	}
	if inspection.Text != "" {
		t.Fatalf("ELF was exposed as text")
	}
}

func TestInspectPayloadFileRejectsUnknownMember(t *testing.T) {
	deb := assembleSampleDeb(t)
	if _, err := InspectPayloadFile(deb, "sample.deb", "usr/bin/not-present"); err == nil {
		t.Fatal("expected unknown member to fail")
	}
}

func TestInspectPayloadFileFollowsHardlink(t *testing.T) {
	archive := filepath.Join(t.TempDir(), "payload.tar")
	writeTar(t, archive, []tarEntry{
		{Name: "usr/lib/libsample.so.1", Mode: 0o755, Body: []byte("library contents")},
		{Name: "usr/lib/libsample.so", Mode: 0o755, Type: tar.TypeLink, Link: "usr/lib/libsample.so.1"},
	})
	inspection, err := InspectPayloadFile(archive, "payload.tar", "usr/lib/libsample.so")
	if err != nil {
		t.Fatal(err)
	}
	if inspection.Path != "usr/lib/libsample.so" ||
		inspection.HardlinkTarget != "usr/lib/libsample.so.1" ||
		inspection.Text != "library contents" || inspection.SHA256 == "" {
		t.Fatalf("hardlink inspection %+v", inspection)
	}
}

func TestInspectPayloadFileSpillsLargeMemberAndKeepsRealHash(t *testing.T) {
	body := bytes.Repeat([]byte("PacSmith spill fixture\n"), 64)
	archive := filepath.Join(t.TempDir(), "payload.tar")
	spillDir := t.TempDir()
	writeTar(t, archive, []tarEntry{{
		Name: "usr/lib/large-fixture", Mode: 0o755, Body: body,
	}})

	inspection, err := inspectPayloadFileWithOptions(
		archive, "payload.tar", "usr/lib/large-fixture",
		payloadInspectionOptions{
			maxInMemoryBytes: 64,
			maxTotalBytes:    1 << 20,
			tempDir:          spillDir,
		},
	)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(body)
	emptySum := sha256.Sum256(nil)
	if inspection.SHA256 != hex.EncodeToString(sum[:]) ||
		inspection.SHA256 == hex.EncodeToString(emptySum[:]) {
		t.Fatalf("spilled content identity %+v", inspection)
	}
	if inspection.Text != string(body) || inspection.TextTruncated ||
		inspection.InspectionNotice != "" {
		t.Fatalf("spilled content inspection %+v", inspection)
	}
	entries, err := os.ReadDir(spillDir)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 0 {
		t.Fatalf("temporary inspection files remain: %+v", entries)
	}
}

func TestInspectPayloadFileSpillsELFMember(t *testing.T) {
	truePath, err := exec.LookPath("true")
	if err != nil {
		t.Skip("true not found")
	}
	body, err := os.ReadFile(truePath)
	if err != nil {
		t.Fatal(err)
	}
	archive := filepath.Join(t.TempDir(), "payload.tar")
	writeTar(t, archive, []tarEntry{{Name: "usr/bin/large-elf", Mode: 0o755, Body: body}})

	inspection, err := inspectPayloadFileWithOptions(
		archive, "payload.tar", "usr/bin/large-elf",
		payloadInspectionOptions{
			maxInMemoryBytes: 1,
			maxTotalBytes:    int64(len(body)) + 1,
			tempDir:          t.TempDir(),
		},
	)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(body)
	if inspection.SHA256 != hex.EncodeToString(sum[:]) || inspection.ELF == nil ||
		inspection.MagicHex[:8] != "7f454c46" {
		t.Fatalf("spilled ELF inspection %+v", inspection)
	}
}

func TestInspectPayloadFileBeyondHardLimitHasNoFalseHash(t *testing.T) {
	body := bytes.Repeat([]byte("too large"), 16)
	archive := filepath.Join(t.TempDir(), "payload.tar")
	writeTar(t, archive, []tarEntry{{Name: "usr/lib/too-large", Body: body}})

	inspection, err := inspectPayloadFileWithOptions(
		archive, "payload.tar", "usr/lib/too-large",
		payloadInspectionOptions{maxInMemoryBytes: 32, maxTotalBytes: int64(len(body) - 1)},
	)
	if err != nil {
		t.Fatal(err)
	}
	if inspection.SHA256 != "" || inspection.InspectionNotice == "" {
		t.Fatalf("hard-limit inspection must omit an unknown hash: %+v", inspection)
	}
}
