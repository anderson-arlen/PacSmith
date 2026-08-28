package updatecheck

import (
	"bytes"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
)

func TestAPTMetadataSelection(t *testing.T) {
	zeroDigest := hex.EncodeToString(make([]byte, sha256.Size))
	index := []byte("Package: demo\nVersion: 1.9-1\nArchitecture: amd64\nFilename: pool/demo-old.deb\nSHA256: " +
		zeroDigest + "\nSize: 12\n\n" +
		"Package: demo\nVersion: 2.0-1\nArchitecture: amd64\nFilename: pool/demo.deb\nSHA256: " +
		zeroDigest + "\nSize: 15\n")
	digest := sha256.Sum256(index)
	release := []byte(fmt.Sprintf("Origin: Test\nSHA256:\n %s %d main/binary-amd64/Packages\n",
		hex.EncodeToString(digest[:]), len(index)))
	entries, err := parseAPTRelease(release)
	if err != nil {
		t.Fatal(err)
	}
	selectedIndex, err := selectAPTIndex(entries, "main", "amd64", false)
	if err != nil || selectedIndex.Size != int64(len(index)) {
		t.Fatalf("index %+v: %v", selectedIndex, err)
	}
	selected, err := latestAPTPackage(index, "demo", "amd64")
	if err != nil {
		t.Fatal(err)
	}
	if selected.Version != "2.0-1" || selected.Filename != "pool/demo.deb" {
		t.Fatalf("selected %+v", selected)
	}
}

func TestRPMMetadataSelection(t *testing.T) {
	zeroDigest := hex.EncodeToString(make([]byte, sha256.Size))
	repomd := []byte("<repomd xmlns=\"http://linux.duke.edu/metadata/repo\">" +
		"<data type=\"primary\"><checksum type=\"sha256\">" + zeroDigest +
		"</checksum><location href=\"repodata/primary.xml.gz\"/></data></repomd>")
	primary, err := parseRPMRepomd(repomd)
	if err != nil {
		t.Fatal(err)
	}
	if primary.Path != "repodata/primary.xml.gz" {
		t.Fatalf("primary %+v", primary)
	}
	primaryXML := []byte("<metadata xmlns=\"http://linux.duke.edu/metadata/common\">" +
		"<package><name>demo</name><arch>x86_64</arch><version epoch=\"0\" ver=\"1.0\" rel=\"2\"/>" +
		"<checksum type=\"sha256\">" + zeroDigest +
		"</checksum><location href=\"Packages/demo-1.rpm\"/></package>" +
		"<package><name>demo</name><arch>x86_64</arch><version epoch=\"0\" ver=\"1.0\" rel=\"10\"/>" +
		"<checksum type=\"sha256\">" + zeroDigest +
		"</checksum><location href=\"Packages/demo-2.rpm\"/></package></metadata>")
	selected, err := latestRPMPackage(primaryXML, "demo", "x86_64")
	if err != nil {
		t.Fatal(err)
	}
	if selected.evr() != "1.0-10" || selected.Filename != "Packages/demo-2.rpm" {
		t.Fatalf("selected %+v", selected)
	}
}

func TestRPMMetadataSelectionAcceptsSHA1Alias(t *testing.T) {
	digest := hex.EncodeToString(make([]byte, sha1.Size))
	primaryXML := []byte("<metadata xmlns=\"http://linux.duke.edu/metadata/common\">" +
		"<package><name>slack</name><arch>x86_64</arch>" +
		"<version epoch=\"0\" ver=\"4.51.191\" rel=\"0.1.el8\"/>" +
		"<checksum type=\"sha\">" + digest +
		"</checksum><location href=\"slack-4.51.191-0.1.el8.x86_64.rpm\"/></package></metadata>")
	selected, err := latestRPMPackage(primaryXML, "slack", "x86_64")
	if err != nil {
		t.Fatal(err)
	}
	if selected.evr() != "4.51.191-0.1.el8" || selected.ChecksumType != "sha" {
		t.Fatalf("selected %+v", selected)
	}
}

func TestRPMMetadataReportsUnsupportedPackageChecksum(t *testing.T) {
	primaryXML := []byte("<metadata><package><name>demo</name><arch>x86_64</arch>" +
		"<version epoch=\"0\" ver=\"2.0\" rel=\"1\"/>" +
		"<checksum type=\"md5\">00000000000000000000000000000000</checksum>" +
		"<location href=\"demo-2.0-1.x86_64.rpm\"/></package></metadata>")
	_, err := latestRPMPackage(primaryXML, "demo", "x86_64")
	if err == nil || !strings.Contains(err.Error(), "unsupported checksum type: md5") {
		t.Fatalf("error = %v", err)
	}
}

func TestVerifyDownloadedRPMPackageSHA1(t *testing.T) {
	payload := []byte("package bytes")
	store, err := artifact.New(t.TempDir(), t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	object, err := store.Ingest(bytes.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	expected := sha1.Sum(payload)
	service := &Service{Artifacts: &artifact.Registry{Store: store}}
	if err := service.verifyDownloadedRPMPackageChecksum(object.SHA256, "sha",
		hex.EncodeToString(expected[:])); err != nil {
		t.Fatal(err)
	}
	if err := service.verifyDownloadedRPMPackageChecksum(object.SHA256, "sha",
		strings.Repeat("0", sha1.Size*2)); err == nil {
		t.Fatal("mismatched SHA-1 checksum was accepted")
	}
}
