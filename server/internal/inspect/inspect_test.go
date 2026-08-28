package inspect

import (
	"archive/tar"
	"bytes"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func sampleDebDir(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("could not locate test file")
	}
	dir := filepath.Join(filepath.Dir(file), "..", "..", "..", "client", "tests", "fixtures", "sample-deb")
	if _, err := os.Stat(dir); err != nil {
		t.Fatalf("sample-deb fixture missing: %s", dir)
	}
	return dir
}

func mustLookPath(t *testing.T, name string) string {
	t.Helper()
	path, err := exec.LookPath(name)
	if err != nil {
		t.Fatalf("%s is required: %v", name, err)
	}
	return path
}

func writeFile(t *testing.T, path string, contents []byte, mode os.FileMode) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, contents, mode); err != nil {
		t.Fatal(err)
	}
}

func assembleSampleDeb(t *testing.T) string {
	t.Helper()
	bsdtar := mustLookPath(t, "bsdtar")
	ar := mustLookPath(t, "ar")
	fixture := sampleDebDir(t)
	dir := t.TempDir()
	packageDir := filepath.Join(dir, "package")
	if err := os.MkdirAll(packageDir, 0o755); err != nil {
		t.Fatal(err)
	}
	debianBinary, err := os.ReadFile(filepath.Join(fixture, "debian-binary"))
	if err != nil {
		t.Fatal(err)
	}
	writeFile(t, filepath.Join(packageDir, "debian-binary"), debianBinary, 0o644)

	run := func(name string, args ...string) {
		t.Helper()
		cmd := exec.Command(name, args...)
		cmd.Dir = packageDir
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("%s %s: %v\n%s", name, strings.Join(args, " "), err, out)
		}
	}
	run(bsdtar, "-caf", "control.tar.zst", "-C", filepath.Join(fixture, "control"), ".")
	dataCopy := filepath.Join(dir, "data")
	copyTree(t, filepath.Join(fixture, "data"), dataCopy)
	smoke := filepath.Join(dataCopy, "usr", "bin", "pacsmith-smoke")
	if err := os.Chmod(smoke, 0o755); err != nil {
		t.Fatal(err)
	}
	run(bsdtar, "-caf", "data.tar.zst", "-C", dataCopy, ".")
	run(ar, "r", "sample.deb", "debian-binary", "control.tar.zst", "data.tar.zst")
	return filepath.Join(packageDir, "sample.deb")
}

func copyTree(t *testing.T, src, dst string) {
	t.Helper()
	err := filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
	if err != nil {
		t.Fatal(err)
	}
}

func writeTar(t *testing.T, path string, entries []tarEntry) {
	t.Helper()
	file, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	tw := tar.NewWriter(file)
	for _, entry := range entries {
		hdr := &tar.Header{
			Name:     entry.Name,
			Mode:     entry.Mode,
			Size:     int64(len(entry.Body)),
			Typeflag: entry.Type,
			Linkname: entry.Link,
			Devmajor: entry.DevMajor,
			Devminor: entry.DevMinor,
		}
		if entry.Type == 0 {
			hdr.Typeflag = tar.TypeReg
		}
		if entry.Mode == 0 && hdr.Typeflag == tar.TypeReg {
			hdr.Mode = 0644
		}
		if hdr.Typeflag == tar.TypeDir {
			hdr.Size = 0
			if hdr.Mode == 0 {
				hdr.Mode = 0755
			}
		}
		if hdr.Typeflag == tar.TypeSymlink || hdr.Typeflag == tar.TypeLink ||
			hdr.Typeflag == tar.TypeChar || hdr.Typeflag == tar.TypeBlock || hdr.Typeflag == tar.TypeFifo {
			hdr.Size = 0
		}
		if err := tw.WriteHeader(hdr); err != nil {
			t.Fatal(err)
		}
		if hdr.Size > 0 {
			if _, err := tw.Write(entry.Body); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
}

type tarEntry struct {
	Name     string
	Mode     int64
	Type     byte
	Link     string
	Body     []byte
	DevMajor int64
	DevMinor int64
}

func elfHeader(machine uint16) []byte {
	header := make([]byte, 64)
	copy(header[0:], []byte{0x7f, 'E', 'L', 'F'})
	header[4] = 2
	header[5] = 1
	header[6] = 1
	binary.LittleEndian.PutUint16(header[16:18], 2)
	binary.LittleEndian.PutUint16(header[18:20], machine)
	return header
}

func appendBE32(buf *[]byte, value uint32) {
	var raw [4]byte
	binary.BigEndian.PutUint32(raw[:], value)
	*buf = append(*buf, raw[:]...)
}

func TestNormalizedArchivePath(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name   string
		path   string
		want   string
		wantOK bool
	}{
		{name: "dot-prefix", path: "./usr/bin/tool", want: "usr/bin/tool", wantOK: true},
		{name: "parent", path: "../../etc/passwd", wantOK: false},
		{name: "absolute", path: "/etc/passwd", wantOK: false},
		{name: "nul", path: "usr/bin/\x00tool", wantOK: false},
		{name: "windows-drive", path: `C:\Windows\System32`, wantOK: false},
		{name: "windows-drive-slash", path: "C:/Windows", wantOK: false},
		{name: "dot-entry", path: ".", want: "", wantOK: true},
		{name: "internal-dot", path: "usr/./bin/tool", want: "usr/bin/tool", wantOK: true},
		{name: "internal-parent", path: "usr/../etc/passwd", wantOK: false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, ok := NormalizedArchivePath(tt.path)
			if ok != tt.wantOK {
				t.Fatalf("ok=%v want %v", ok, tt.wantOK)
			}
			if ok && got != tt.want {
				t.Fatalf("path %q, want %q", got, tt.want)
			}
		})
	}
}

func TestSymlinkPolicy(t *testing.T) {
	t.Parallel()
	if !SafeSymlinkTarget("usr/bin/tool", "../lib/tool") {
		t.Fatal("relative link within tree should be accepted")
	}
	if SafeSymlinkTarget("usr/bin/tool", "../../../etc/passwd") {
		t.Fatal("relative escape should be rejected")
	}
	if SafeSymlinkTarget("usr/bin/tool", "/tmp/escape") {
		t.Fatal("absolute relative-policy link should be rejected")
	}
	if !SafePackageSymlinkTarget("etc/cron.daily/brave-browser", "/opt/brave.com/brave/cron/brave-browser") {
		t.Fatal("package absolute /opt link should be accepted")
	}
	if SafePackageSymlinkTarget("usr/bin/tool", "/tmp/escape") {
		t.Fatal("/tmp package link should be rejected")
	}
	if SafePackageSymlinkTarget("usr/bin/tool", "/etc/passwd") {
		t.Fatal("/etc package link should be rejected")
	}
	if SafePackageSymlinkTarget("usr/bin/tool", "/opt/../tmp/escape") {
		t.Fatal("cleaned /opt/../tmp escape should be rejected")
	}
	if SafeAppImageSymlinkTarget("runtime/compat/tmp/state", "/tmp/state") {
		t.Fatal("AppImage /tmp link should be rejected")
	}
	if !SafeAppImageSymlinkTarget("runtime/compat/usr/bin/env", "/usr/bin/env") {
		t.Fatal("AppImage /usr/bin link should be accepted")
	}
	if SafeAppImageSymlinkTarget("runtime/compat/usr/bin/disguised", "/usr/bin/../../tmp/state") {
		t.Fatal("AppImage cleaned escape should be rejected")
	}
}

func TestParseControlAndDependencies(t *testing.T) {
	t.Parallel()
	parsed := parseControlPackage([]byte("Package: Vendor-App\nVersion: 1.2.3-1\nArchitecture: amd64\nX-Vendor-Field: retained\n"))
	if parsed.Package != "Vendor-App" || parsed.Version != "1.2.3-1" {
		t.Fatalf("control parse: %+v", parsed)
	}
	if parsed.RawFields["X-Vendor-Field"] != "retained" {
		t.Fatalf("raw fields: %#v", parsed.RawFields)
	}
	multiline := parseControlPackage([]byte("Package: example\nDescription: Short summary\n long detail\n .\n final paragraph\n"))
	if multiline.Description != "Short summary\nlong detail\n\nfinal paragraph" {
		t.Fatalf("multiline description: %q", multiline.Description)
	}

	deps := ParseDependencies("libgtk-3-0, libnss3:any, vendor-runtime [amd64]")
	if len(deps) != 3 || deps[0].Alternatives[0].PackageName != "libgtk-3-0" ||
		deps[1].Alternatives[0].PackageName != "libnss3" ||
		deps[2].Alternatives[0].PackageName != "vendor-runtime" {
		t.Fatalf("deps: %+v", deps)
	}
	alts := ParseDependencies("libfoo (>= 1.2) | libbar (<< 3:4.0-1), plain")
	if alts[0].Alternatives[0].VersionOperator != ">=" || alts[0].Alternatives[1].VersionOperator != "<<" {
		t.Fatalf("alternatives: %+v", alts)
	}
	mappings := LoadVerifiedMappings()
	if mappings["libgtk-3-0"] != "gtk3" || mappings["libc6"] != "glibc" {
		t.Fatalf("mappings: %#v", mappings)
	}
	_ = ApplyVerifiedMappings(deps, mappings)
	if deps[0].ArchPackage != "gtk3" || deps[0].Status != MappingResolved {
		t.Fatalf("verified mapping not applied: %+v", deps[0])
	}
}

func TestAnalyzeSampleDeb(t *testing.T) {
	deb := assembleSampleDeb(t)
	got, err := Detect(deb)
	if err != nil {
		t.Fatal(err)
	}
	if got != SourceDebian {
		t.Fatalf("Detect=%v", got)
	}
	analysis, err := Analyze(deb)
	if err != nil {
		t.Fatal(err)
	}
	if analysis.Metadata.Package != "pacsmith-smoke" {
		t.Fatalf("package %q", analysis.Metadata.Package)
	}
	if analysis.Metadata.Version != "3:1.2.3~beta1-4" {
		t.Fatalf("version %q", analysis.Metadata.Version)
	}
	resolved := map[string]string{}
	unresolved := false
	for _, dep := range analysis.Dependencies {
		if len(dep.Alternatives) == 0 {
			continue
		}
		if dep.Status == MappingResolved {
			resolved[dep.Alternatives[0].PackageName] = dep.ArchPackage
		}
		if dep.Alternatives[0].PackageName == "unknown-vendor-runtime" {
			if dep.Status != MappingUnresolved {
				t.Fatalf("unknown runtime should stay unresolved: %+v", dep)
			}
			unresolved = true
		}
	}
	if resolved["libc6"] != "glibc" || resolved["libgtk-3-0"] != "gtk3" {
		t.Fatalf("mapped deps: %#v", resolved)
	}
	if !unresolved {
		t.Fatal("missing unresolved vendor runtime")
	}
	if len(analysis.MaintainerScripts) == 0 {
		t.Fatal("expected maintainer scripts as data")
	}
	foundPostinst := false
	for _, script := range analysis.MaintainerScripts {
		if script.Name == "postinst" {
			foundPostinst = true
			if !strings.Contains(script.Contents, "packages.example.invalid") {
				t.Fatalf("postinst contents: %q", script.Contents)
			}
		}
	}
	if !foundPostinst {
		t.Fatal("postinst missing")
	}
	aptExcluded := false
	for _, rule := range analysis.PayloadRules {
		if rule.Path == "etc/apt" && rule.Excluded {
			aptExcluded = true
		}
	}
	if !aptExcluded {
		t.Fatalf("etc/apt should be excluded: %+v", analysis.PayloadRules)
	}
	for _, rule := range analysis.PayloadRules {
		if rule.Path == "etc/apt" && rule.AcknowledgedFingerprint == "" {
			t.Fatal("etc/apt default exclusion should bind a content fingerprint")
		}
	}
	if analysis.Icon == nil || analysis.Icon.SourcePath != "usr/share/pixmaps/pacsmith-smoke.xpm" {
		t.Fatalf("icon: %+v", analysis.Icon)
	}
	if analysis.Install.Icon.ProjectPath != "files/integration/icon.xpm" {
		t.Fatalf("icon project path: %+v", analysis.Install.Icon)
	}
	if analysis.Install.Icon.SourceKind != IconPayload {
		t.Fatalf("icon source kind: %+v", analysis.Install.Icon)
	}
	foundURL := false
	for _, url := range analysis.UpdateCandidates {
		if strings.Contains(url, "packages.example.invalid") {
			foundURL = true
		}
	}
	if !foundURL {
		t.Fatalf("update candidates: %#v", analysis.UpdateCandidates)
	}
	if len(analysis.AptCandidates) == 0 {
		t.Fatal("expected APT repository candidate from vendor sources")
	}
	if analysis.AptCandidates[0].URI != "https://packages.example.invalid/apt" {
		t.Fatalf("apt candidate: %+v", analysis.AptCandidates[0])
	}
}

func TestReadPayloadFile(t *testing.T) {
	deb := assembleSampleDeb(t)
	got, err := ReadPayloadFile(deb, "usr/share/pixmaps/pacsmith-smoke.xpm", maxIconBytes)
	if err != nil {
		t.Fatal(err)
	}
	if !validIconContents("usr/share/pixmaps/pacsmith-smoke.xpm", got) {
		t.Fatalf("payload icon was not valid: %d bytes", len(got))
	}
	if _, err := ReadPayloadFile(deb, "usr/share/pixmaps/missing.png", maxIconBytes); err == nil {
		t.Fatal("expected missing payload file to fail")
	}
}

func TestSelectsLargestVendorBundleIcon(t *testing.T) {
	png := func(size uint32) []byte {
		contents := make([]byte, 24)
		copy(contents, pngSignature)
		binary.BigEndian.PutUint32(contents[16:20], size)
		binary.BigEndian.PutUint32(contents[20:24], size)
		return contents
	}
	candidates := []iconCandidate{
		{path: "opt/vendor/product_logo_16.png", contents: png(16)},
		{path: "opt/vendor/product_logo_256.png", contents: png(256)},
	}
	if !isDebIconCandidate(candidates[0].path, int64(len(candidates[0].contents))) {
		t.Fatal("vendor-bundled PNG should be considered as a DEB icon")
	}
	selected := selectBestIcon(candidates, map[string]struct{}{"vendor": {}}, "vendor")
	if selected == nil || selected.path != candidates[1].path {
		t.Fatalf("selected icon = %+v, want %q", selected, candidates[1].path)
	}
}

func TestVSCodePostinstAptRepository(t *testing.T) {
	script := MaintainerScript{
		Name: "postinst",
		Contents: `#!/bin/sh
set -e
CODE_TRUSTED_PART=/usr/share/keyrings/microsoft.gpg
cat > /etc/apt/sources.list.d/vscode.sources << EOF
Types: deb
URIs: https://packages.microsoft.com/repos/code
Suites: stable
Components: main
Architectures: amd64
Signed-By: $CODE_TRUSTED_PART
EOF
# Sourced from https://packages.microsoft.com/keys/microsoft.asc
`,
	}
	apt, _, _ := ScriptEvidence([]MaintainerScript{script})
	if len(apt) != 1 {
		t.Fatalf("apt candidates: %+v", apt)
	}
	if apt[0].URI != "https://packages.microsoft.com/repos/code" || apt[0].Suite != "stable" {
		t.Fatalf("parsed %+v", apt[0])
	}
	if len(apt[0].Components) != 1 || apt[0].Components[0] != "main" {
		t.Fatalf("components %+v", apt[0].Components)
	}
}

func TestELFNotExecuted(t *testing.T) {
	dir := t.TempDir()
	marker := filepath.Join(dir, "executed")
	payload := elfHeader(62)
	payload = append(payload, []byte("#!/bin/sh\necho executed > "+marker+"\n")...)
	path := filepath.Join(dir, "vendorctl-2.0.0-linux-x86_64")
	writeFile(t, path, payload, 0o755)

	got, err := Detect(path)
	if err != nil {
		t.Fatal(err)
	}
	if got != SourceELF {
		t.Fatalf("Detect=%v", got)
	}
	analysis, err := Analyze(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatal("ELF payload was executed")
	}
	if analysis.Type != SourceELF {
		t.Fatalf("type %v", analysis.Type)
	}
	if analysis.Metadata.Package != "vendorctl" || analysis.Metadata.Version != "2.0.0" {
		t.Fatalf("inferred identity %+v", analysis.Metadata)
	}
	if !strings.HasPrefix(analysis.Install.BinaryDestination, "/usr/bin/") {
		t.Fatalf("destination %q", analysis.Install.BinaryDestination)
	}

	truePath, err := exec.LookPath("true")
	if err != nil {
		t.Skip("true not found")
	}
	host, err := Analyze(truePath)
	if err != nil {
		t.Fatal(err)
	}
	if host.Type != SourceELF || len(host.Install.Launchers) != 1 {
		t.Fatalf("host true analysis: %+v", host)
	}
}

func TestAnalyzeArtifactUsesOriginalFilenameForRawIdentity(t *testing.T) {
	dir := t.TempDir()
	storedPath := filepath.Join(dir, strings.Repeat("f", 64))
	writeFile(t, storedPath, elfHeader(62), 0o755)

	analysis, err := AnalyzeArtifact(storedPath, "chamber-v3.1.5-linux-amd64")
	if err != nil {
		t.Fatal(err)
	}
	if analysis.Metadata.Package != "chamber" || analysis.Metadata.Version != "3.1.5" {
		t.Fatalf("inferred identity %+v", analysis.Metadata)
	}
	if analysis.Install.BinarySourcePath != "chamber-v3.1.5-linux-amd64" {
		t.Fatalf("binary source path %q", analysis.Install.BinarySourcePath)
	}
	if len(analysis.Install.Launchers) != 1 || analysis.Install.Launchers[0].CommandName != "chamber" {
		t.Fatalf("launchers %+v", analysis.Install.Launchers)
	}
	if len(analysis.Payload) != 1 || analysis.Payload[0].Path != "chamber-v3.1.5-linux-amd64" {
		t.Fatalf("payload %+v", analysis.Payload)
	}
}

func TestInferNameVersionHandlesRawArtifactFilenames(t *testing.T) {
	tests := []struct {
		filename string
		pkg      string
		version  string
	}{
		{"Chirp-next-20260814-x86_64.appimage", "chirp-next", "20260814"},
		{"UltiMaker-Cura-5.13.0-linux-X64.AppImage", "ultimaker-cura", "5.13.0"},
		{"chamber-v3.1.5-linux-amd64", "chamber", "3.1.5"},
	}
	for _, tc := range tests {
		t.Run(tc.filename, func(t *testing.T) {
			var metadata Metadata
			inferNameVersion(tc.filename, &metadata)
			if metadata.Package != tc.pkg || metadata.Version != tc.version {
				t.Fatalf("inferred identity %+v, want package %q version %q", metadata, tc.pkg, tc.version)
			}
		})
	}
}

func TestRejectsUnsafeArchivePaths(t *testing.T) {
	dir := t.TempDir()
	tests := []struct {
		name    string
		entries []tarEntry
		want    string
	}{
		{
			name:    "path-traversal",
			entries: []tarEntry{{Name: "../../etc/passwd", Body: []byte("secret")}},
			want:    "Unsafe archive path",
		},
		{
			name:    "absolute",
			entries: []tarEntry{{Name: "/etc/passwd", Body: []byte("secret")}},
			want:    "Unsafe archive path",
		},
		{
			name:    "device",
			entries: []tarEntry{{Name: "dev/null", Type: tar.TypeChar, Mode: 0666, DevMajor: 1, DevMinor: 3}},
			want:    "Special device entry",
		},
		{
			name:    "fifo",
			entries: []tarEntry{{Name: "run/pipe", Type: tar.TypeFifo, Mode: 0644}},
			want:    "Special device entry",
		},
		{
			name: "unsafe-hardlink",
			entries: []tarEntry{
				{Name: "usr/bin/tool", Body: []byte("ok")},
				{Name: "usr/bin/evil", Type: tar.TypeLink, Link: "../../etc/passwd"},
			},
			want: "Unsafe hard link",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			path := filepath.Join(dir, tt.name+".tar")
			writeTar(t, path, tt.entries)
			_, err := Analyze(path)
			if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("error %v, want substring %q", err, tt.want)
			}
		})
	}
}

func TestUnsafeArchiveSymlinkIsReviewedNotFatal(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "links.tar")
	writeTar(t, path, []tarEntry{
		{Name: "opt/vendor/cron/vendor", Mode: 0755, Body: []byte("#!/bin/sh\nexit 0\n")},
		{Name: "etc/cron.daily/vendor", Type: tar.TypeSymlink, Link: "/opt/vendor/cron/vendor"},
		{Name: "usr/bin/evil", Type: tar.TypeSymlink, Link: "/tmp/escape"},
	})
	analysis, err := Analyze(path)
	if err != nil {
		t.Fatal(err)
	}
	var cron, evil *PayloadEntry
	for i := range analysis.Payload {
		entry := &analysis.Payload[i]
		switch entry.Path {
		case "etc/cron.daily/vendor":
			cron = entry
		case "usr/bin/evil":
			evil = entry
		}
	}
	if cron == nil || cron.SymlinkTarget != "/opt/vendor/cron/vendor" || !cron.RequiresReview {
		t.Fatalf("cron entry: %+v", cron)
	}
	if evil == nil || evil.SymlinkTarget != "/tmp/escape" || !evil.RequiresReview {
		t.Fatalf("evil entry: %+v", evil)
	}
	excluded := false
	for _, rule := range analysis.PayloadRules {
		if rule.Path == "usr/bin/evil" && rule.Excluded {
			excluded = true
		}
	}
	if !excluded {
		t.Fatalf("rules: %+v", analysis.PayloadRules)
	}
}

func TestType1AppImageRejected(t *testing.T) {
	path := filepath.Join(t.TempDir(), "vendor.AppImage")
	header := make([]byte, 12)
	copy(header, []byte{0x7f, 'E', 'L', 'F'})
	header[8] = 'A'
	header[9] = 'I'
	header[10] = 1
	writeFile(t, path, header, 0o644)
	_, err := Detect(path)
	if err == nil || !strings.Contains(err.Error(), "Type 1") {
		t.Fatalf("Detect error %v", err)
	}
}

func TestType2AppImageDetectedBeforeELF(t *testing.T) {
	path := filepath.Join(t.TempDir(), "vendor.AppImage")
	header := make([]byte, 12)
	copy(header, []byte{0x7f, 'E', 'L', 'F'})
	header[8] = 'A'
	header[9] = 'I'
	header[10] = 2
	writeFile(t, path, header, 0o644)
	got, err := Detect(path)
	if err != nil {
		t.Fatal(err)
	}
	if got != SourceAppImage {
		t.Fatalf("Detect=%v", got)
	}
	_, err = Analyze(path)
	if err == nil || !(strings.Contains(err.Error(), "AppImage") &&
		(strings.Contains(err.Error(), "SquashFS") || strings.Contains(err.Error(), "squashfs-tools"))) {
		t.Fatalf("Analyze error %v", err)
	}
}

func TestRPMHeaderWithoutRpmCommand(t *testing.T) {
	type rpmIndex struct {
		tag, typ, offset, count uint32
	}
	var entries []rpmIndex
	var store []byte
	addString := func(tag uint32, value string, typ uint32) {
		offset := uint32(len(store))
		store = append(store, value...)
		store = append(store, 0)
		entries = append(entries, rpmIndex{tag, typ, offset, 1})
	}
	addStrings := func(tag uint32, values []string) {
		offset := uint32(len(store))
		for _, value := range values {
			store = append(store, value...)
			store = append(store, 0)
		}
		entries = append(entries, rpmIndex{tag, 8, offset, uint32(len(values))})
	}
	addIntegers := func(tag uint32, values []uint32) {
		offset := uint32(len(store))
		for _, value := range values {
			appendBE32(&store, value)
		}
		entries = append(entries, rpmIndex{tag, 4, offset, uint32(len(values))})
	}
	addString(1000, "vendor-rpm", 6)
	addString(1001, "2.4.1", 6)
	addString(1002, "3.el9", 6)
	addIntegers(1003, []uint32{2})
	addString(1004, "Vendor RPM application", 9)
	addString(1005, "Long RPM description", 9)
	addString(1011, "Example Vendor", 6)
	addString(1020, "https://vendor.example/rpm", 6)
	addString(1022, "x86_64", 6)
	addString(1024, "update-desktop-database || true", 6)
	addStrings(1049, []string{"gtk3", "rpmlib(CompressedFileNames)", "/bin/sh"})
	addIntegers(1048, []uint32{0, 0, 1 << 10})
	addStrings(1050, []string{"", "3.0.4-1", ""})
	addStrings(5046, []string{"optional-helper"})
	addStrings(5047, []string{"2.0"})
	addIntegers(5048, []uint32{(1 << 2) | (1 << 3)})
	addStrings(5066, []string{"update-mime-database /usr/share/mime || true"})
	addStrings(5067, []string{"/bin/sh"})
	addIntegers(1116, []uint32{0})
	addStrings(1117, []string{"vendor-helper"})
	addStrings(1118, []string{"/usr/bin/"})
	addStrings(5010, []string{"cap_net_bind_service=ep"})

	buildHeader := func(headerEntries []rpmIndex, headerStore []byte) []byte {
		var header []byte
		header = append(header, 0x8e, 0xad, 0xe8, 0x01)
		appendBE32(&header, 0)
		appendBE32(&header, uint32(len(headerEntries)))
		appendBE32(&header, uint32(len(headerStore)))
		for _, entry := range headerEntries {
			appendBE32(&header, entry.tag)
			appendBE32(&header, entry.typ)
			appendBE32(&header, entry.offset)
			appendBE32(&header, entry.count)
		}
		return append(header, headerStore...)
	}
	rpm := bytes.Repeat([]byte{0}, 96)
	copy(rpm, []byte{0xed, 0xab, 0xee, 0xdb})
	rpm = append(rpm, buildHeader(nil, nil)...)
	rpm = append(rpm, buildHeader(entries, store)...)
	path := filepath.Join(t.TempDir(), "vendor.rpm")
	writeFile(t, path, rpm, 0o644)

	got, err := Detect(path)
	if err != nil {
		t.Fatal(err)
	}
	if got != SourceRPM {
		t.Fatalf("Detect=%v", got)
	}
	analysis, err := Analyze(path)
	if err != nil {
		t.Fatal(err)
	}
	if analysis.Metadata.Package != "vendor-rpm" || analysis.Metadata.Version != "2:2.4.1-3.el9" {
		t.Fatalf("metadata %+v", analysis.Metadata)
	}
	if len(analysis.Dependencies) != 1 || analysis.Dependencies[0].ArchPackage != "gtk3" {
		t.Fatalf("deps %+v", analysis.Dependencies)
	}
	if analysis.Metadata.Recommends != "optional-helper (>= 2.0)" {
		t.Fatalf("recommends %q", analysis.Metadata.Recommends)
	}
	if len(analysis.MaintainerScripts) != 2 {
		t.Fatalf("scripts %+v", analysis.MaintainerScripts)
	}
}

func TestInferArchiveLaunchers(t *testing.T) {
	mapping := InstallMapping{
		ArchiveLayout:     LayoutOptBundle,
		OptDirectory:      "letos",
		CommonPrefix:      "Letos",
		StripCommonPrefix: true,
		DesktopEntries: []DesktopEntry{{
			Enabled:  true,
			Contents: "[Desktop Entry]\nType=Application\nName=Letos\nExec=letos %f\nIcon=letos\n",
		}},
	}
	payload := []PayloadEntry{
		{Path: "Letos/letos", Type: "file", Size: 211976},
		{Path: "Letos/letos.desktop", Type: "file", Size: 80},
		{Path: "Letos/imageformats/libqjpeg.so", Type: "file", Size: 4096, Executable: true},
	}
	if !InferArchiveLaunchers(&mapping, payload, "letos") {
		t.Fatal("expected launcher inference to change mapping")
	}
	if len(mapping.Launchers) != 1 || mapping.Launchers[0].SourcePath != "Letos/letos" {
		t.Fatalf("launchers %+v", mapping.Launchers)
	}
}

func TestDebDeclaredOptCommandWithoutExecutingScript(t *testing.T) {
	bsdtar := mustLookPath(t, "bsdtar")
	ar := mustLookPath(t, "ar")
	root := t.TempDir()
	writeFile(t, filepath.Join(root, "control", "control"), []byte("Package: signal-fixture\nVersion: 1.0\nArchitecture: amd64\nDescription: Fixture\n"), 0o644)
	writeFile(t, filepath.Join(root, "control", "postinst"), []byte("#!/bin/sh\nupdate-alternatives --install '/usr/bin/signal-fixture' 'signal-fixture' '/opt/SignalFixture/signal-fixture' 100 || ln -sf '/opt/SignalFixture/signal-fixture' '/usr/bin/signal-fixture'\n"), 0o644)
	bin := filepath.Join(root, "data", "opt", "SignalFixture", "signal-fixture")
	writeFile(t, bin, []byte("#!/bin/sh\nexit 0\n"), 0o755)
	writeFile(t, filepath.Join(root, "data", "usr", "share", "applications", "signal-fixture.desktop"), []byte("[Desktop Entry]\nType=Application\nName=Signal Fixture\nExec=signal-fixture\n"), 0o644)
	writeFile(t, filepath.Join(root, "debian-binary"), []byte("2.0\n"), 0o644)
	run := func(args ...string) {
		cmd := exec.Command(args[0], args[1:]...)
		cmd.Dir = root
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("%v: %v\n%s", args, err, out)
		}
	}
	run(bsdtar, "-cf", filepath.Join(root, "control.tar"), "-C", filepath.Join(root, "control"), ".")
	run(bsdtar, "-cf", filepath.Join(root, "data.tar"), "-C", filepath.Join(root, "data"), ".")
	run(ar, "r", "signal-fixture_1.0_amd64.deb", "debian-binary", "control.tar", "data.tar")
	analysis, err := Analyze(filepath.Join(root, "signal-fixture_1.0_amd64.deb"))
	if err != nil {
		t.Fatal(err)
	}
	var launcher *LauncherMapping
	for i := range analysis.Install.Launchers {
		if analysis.Install.Launchers[i].SourcePath == "opt/SignalFixture/signal-fixture" {
			launcher = &analysis.Install.Launchers[i]
		}
	}
	if launcher == nil || !launcher.Enabled || launcher.Destination != "/usr/bin/signal-fixture" {
		t.Fatalf("launcher %+v", analysis.Install.Launchers)
	}
	if !strings.Contains(launcher.Provenance.Rationale, "maintainer script explicitly exposes") {
		t.Fatalf("rationale %q", launcher.Provenance.Rationale)
	}
}

func TestSetuidArchiveEntryRequiresReview(t *testing.T) {
	path := filepath.Join(t.TempDir(), "suid.tar")
	writeTar(t, path, []tarEntry{
		{Name: "usr/bin/tool", Mode: 04755, Body: []byte("fixture")},
	})
	analysis, err := Analyze(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(analysis.Payload) == 0 || !analysis.Payload[0].RequiresReview ||
		!strings.Contains(analysis.Payload[0].ReviewReason, "Set-user-ID") {
		t.Fatalf("payload %+v", analysis.Payload)
	}
}

func TestAppImageRuntimeSymlinkWithoutExecutingAppRun(t *testing.T) {
	mksquashfs, err := exec.LookPath("mksquashfs")
	if err != nil {
		t.Skip("squashfs-tools is required for AppImage inspection tests")
	}
	if _, err := exec.LookPath("unsquashfs"); err != nil {
		t.Skip("squashfs-tools is required for AppImage inspection tests")
	}

	root := t.TempDir()
	appDir := filepath.Join(root, "AppDir")
	writeFile(t, filepath.Join(appDir, "AppRun"), []byte("#!/bin/sh\nexit 0\n"), 0o755)
	writeFile(t, filepath.Join(appDir, "vendor-tool.desktop"), []byte("[Desktop Entry]\nType=Application\nName=Vendor Tool\nExec=vendor-tool %U\nIcon=vendor-tool\n"), 0o644)
	if err := os.MkdirAll(filepath.Join(appDir, "runtime/compat/usr/bin"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("/usr/bin/env", filepath.Join(appDir, "runtime/compat/usr/bin/env")); err != nil {
		t.Fatal(err)
	}

	squashfs := filepath.Join(root, "payload.squashfs")
	pack := exec.Command(mksquashfs, appDir, squashfs, "-noappend", "-processors", "1", "-quiet")
	if out, err := pack.CombinedOutput(); err != nil {
		t.Fatalf("mksquashfs: %v\n%s", err, out)
	}
	payload, err := os.ReadFile(squashfs)
	if err != nil {
		t.Fatal(err)
	}
	header := make([]byte, 4096)
	copy(header, []byte{0x7f, 'E', 'L', 'F'})
	header[8] = 'A'
	header[9] = 'I'
	header[10] = 2
	appImage := filepath.Join(root, "VendorTool-1.0-x86_64.AppImage")
	writeFile(t, appImage, append(header, payload...), 0o644)

	analysis, err := Analyze(appImage)
	if err != nil {
		t.Fatal(err)
	}
	if analysis.Type != SourceAppImage {
		t.Fatalf("type %v", analysis.Type)
	}
	var link *PayloadEntry
	for i := range analysis.Payload {
		if analysis.Payload[i].Path == "runtime/compat/usr/bin/env" {
			link = &analysis.Payload[i]
		}
	}
	if link == nil || link.Type != "symlink" || link.SymlinkTarget != "/usr/bin/env" {
		t.Fatalf("runtime symlink: %+v", link)
	}
	if len(analysis.Install.Launchers) != 1 || analysis.Install.Launchers[0].SourcePath != "AppRun" {
		t.Fatalf("launchers %+v", analysis.Install.Launchers)
	}
	if analysis.Install.Launchers[0].CommandName != "vendor-tool" {
		t.Fatalf("command %q", analysis.Install.Launchers[0].CommandName)
	}
	if !analysis.Install.AppRun.Present || !analysis.Install.AppRun.Script {
		t.Fatalf("AppRun %+v", analysis.Install.AppRun)
	}
	var appRun *PayloadEntry
	for i := range analysis.Payload {
		if analysis.Payload[i].Path == "AppRun" {
			appRun = &analysis.Payload[i]
		}
	}
	if appRun == nil || !appRun.Executable || appRun.TextPreview != "#!/bin/sh\nexit 0\n" ||
		appRun.ContentSHA256 == "" || appRun.ContentSHA256 != analysis.Install.AppRun.OriginalContentsSHA256 {
		t.Fatalf("AppRun payload %+v sha %q", appRun, analysis.Install.AppRun.OriginalContentsSHA256)
	}
}

func TestAppImageSymlinkAppRunTargetsExecutablePayload(t *testing.T) {
	if _, err := exec.LookPath("mksquashfs"); err != nil {
		t.Skip("squashfs-tools is required for AppImage inspection tests")
	}
	if _, err := exec.LookPath("unsquashfs"); err != nil {
		t.Skip("squashfs-tools is required for AppImage inspection tests")
	}

	root := t.TempDir()
	appDir := filepath.Join(root, "AppDir")
	writeFile(t, filepath.Join(appDir, "Letos", "letos"),
		[]byte("#!/bin/sh\nexit 0\n"), 0o755)
	if err := os.Symlink("Letos/letos", filepath.Join(appDir, "AppRun")); err != nil {
		t.Fatal(err)
	}
	writeFile(t, filepath.Join(appDir, "letos.desktop"),
		[]byte("[Desktop Entry]\nType=Application\nName=Letos\nExec=letos\nIcon=letos\n"), 0o644)

	squashfs := filepath.Join(root, "payload.squashfs")
	pack := exec.Command("mksquashfs", appDir, squashfs, "-noappend", "-processors", "1", "-quiet")
	if out, err := pack.CombinedOutput(); err != nil {
		t.Fatalf("mksquashfs: %v\n%s", err, out)
	}
	payload, err := os.ReadFile(squashfs)
	if err != nil {
		t.Fatal(err)
	}
	header := make([]byte, 4096)
	copy(header, []byte{0x7f, 'E', 'L', 'F'})
	header[8] = 'A'
	header[9] = 'I'
	header[10] = 2
	appImage := filepath.Join(root, "Letos-4.0.3-x86_64.AppImage")
	writeFile(t, appImage, append(header, payload...), 0o644)

	analysis, err := Analyze(appImage)
	if err != nil {
		t.Fatal(err)
	}
	if !analysis.Install.AppRun.Present || analysis.Install.AppRun.Script ||
		analysis.Install.AppRun.ReviewReason != "Symlink AppRun; not a text script" {
		t.Fatalf("AppRun %+v", analysis.Install.AppRun)
	}
	var appRun *PayloadEntry
	for i := range analysis.Payload {
		if analysis.Payload[i].Path == "AppRun" {
			appRun = &analysis.Payload[i]
		}
	}
	if appRun == nil || appRun.Type != "symlink" || appRun.SymlinkTarget != "Letos/letos" ||
		!appRun.Executable {
		t.Fatalf("AppRun payload %+v", appRun)
	}
	if len(analysis.Install.Launchers) != 1 ||
		analysis.Install.Launchers[0].SourcePath != "AppRun" {
		t.Fatalf("launchers %+v", analysis.Install.Launchers)
	}
}

func TestExecutableAppRunSymlinkDoesNotEscapeAppDir(t *testing.T) {
	root := t.TempDir()
	writeFile(t, filepath.Join(root, "bin", "tool"), []byte("fixture"), 0o755)
	if err := os.Symlink("bin/tool", filepath.Join(root, "AppRun")); err != nil {
		t.Fatal(err)
	}
	if !executableAppRunSymlink(root, filepath.Join(root, "AppRun")) {
		t.Fatal("safe executable AppRun symlink was rejected")
	}
	if err := os.Remove(filepath.Join(root, "AppRun")); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("/usr/bin/env", filepath.Join(root, "AppRun")); err != nil {
		t.Fatal(err)
	}
	if executableAppRunSymlink(root, filepath.Join(root, "AppRun")) {
		t.Fatal("AppRun symlink escaping the AppDir was accepted")
	}
}

func TestAppImageCompiledAppRunIsNotAScript(t *testing.T) {
	if _, err := exec.LookPath("mksquashfs"); err != nil {
		t.Skip("squashfs-tools is required for AppImage inspection tests")
	}
	if _, err := exec.LookPath("unsquashfs"); err != nil {
		t.Skip("squashfs-tools is required for AppImage inspection tests")
	}

	root := t.TempDir()
	appDir := filepath.Join(root, "AppDir")
	elf := make([]byte, 64)
	copy(elf, []byte{0x7f, 'E', 'L', 'F'})
	elf[4] = 2
	writeFile(t, filepath.Join(appDir, "AppRun"), elf, 0o755)
	writeFile(t, filepath.Join(appDir, "vendor-tool.desktop"), []byte("[Desktop Entry]\nType=Application\nName=Vendor Tool\nExec=vendor-tool\nIcon=vendor-tool\n"), 0o644)

	squashfs := filepath.Join(root, "payload.squashfs")
	pack := exec.Command("mksquashfs", appDir, squashfs, "-noappend", "-processors", "1", "-quiet")
	if out, err := pack.CombinedOutput(); err != nil {
		t.Fatalf("mksquashfs: %v\n%s", err, out)
	}
	payload, err := os.ReadFile(squashfs)
	if err != nil {
		t.Fatal(err)
	}
	header := make([]byte, 4096)
	copy(header, []byte{0x7f, 'E', 'L', 'F'})
	header[8] = 'A'
	header[9] = 'I'
	header[10] = 2
	appImage := filepath.Join(root, "VendorTool-1.0-x86_64.AppImage")
	writeFile(t, appImage, append(header, payload...), 0o644)

	analysis, err := Analyze(appImage)
	if err != nil {
		t.Fatal(err)
	}
	if !analysis.Install.AppRun.Present || analysis.Install.AppRun.Script {
		t.Fatalf("compiled AppRun treated as script: %+v", analysis.Install.AppRun)
	}
	if analysis.Install.AppRun.Contents != "" {
		t.Fatalf("compiled AppRun stored text contents %q", analysis.Install.AppRun.Contents)
	}
	if analysis.Install.AppRun.ReviewReason != "Compiled ELF entry point" {
		t.Fatalf("review reason %q", analysis.Install.AppRun.ReviewReason)
	}
	sum := sha256Hex(elf)
	if analysis.Install.AppRun.OriginalContentsSHA256 != sum {
		t.Fatalf("AppRun sha %q want %q", analysis.Install.AppRun.OriginalContentsSHA256, sum)
	}
	var stored *PayloadEntry
	for i := range analysis.Payload {
		if analysis.Payload[i].Path == "AppRun" {
			stored = &analysis.Payload[i]
		}
	}
	if stored == nil || !stored.Executable || stored.TextPreview != "" || stored.ContentSHA256 != sum {
		t.Fatalf("compiled AppRun payload %+v", stored)
	}
}
