package recipe

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSanitizePackageName(t *testing.T) {
	cases := []struct {
		in, want string
	}{
		{"Some Vendor Tool", "some-vendor-tool"},
		{"--Hello!!World--", "hello-world"},
		{"libfoo++_bin", "libfoo++_bin"},
		{"!!!", "vendor-package-bin"},
	}
	for _, tc := range cases {
		if got := SanitizePackageName(tc.in); got != tc.want {
			t.Errorf("SanitizePackageName(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestTranslateVersion(t *testing.T) {
	cases := []struct {
		in, want string
	}{
		{"1.2.3-4", "1.2.3"},
		{"2:4.5.0-1", "4.5.0"},
		{"1.0~beta1-2", "1.0.beta1"},
	}
	for _, tc := range cases {
		if got := TranslateVersion(tc.in); got != tc.want {
			t.Errorf("TranslateVersion(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
	epoch, version := SplitEpochAndVersion("2:1.2.3-4")
	if epoch != "2" || version != "1.2.3" {
		t.Fatalf("SplitEpochAndVersion(2:1.2.3-4) = (%q, %q)", epoch, version)
	}
}

func TestTranslateArchitecture(t *testing.T) {
	cases := map[string]string{
		"amd64":   "x86_64",
		"arm64":   "aarch64",
		"i386":    "i686",
		"i686":    "i686",
		"all":     "any",
		"noarch":  "any",
		"riscv64": "riscv64",
	}
	for in, want := range cases {
		if got := TranslateArchitecture(in); got != want {
			t.Errorf("TranslateArchitecture(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestShellQuote(t *testing.T) {
	if got := ShellQuote("vendor app"); got != "'vendor app'" {
		t.Fatalf("ShellQuote spaces: %q", got)
	}
	if got := ShellQuote("it's"); got != `'it'"'"'s'` {
		t.Fatalf("ShellQuote quote: %q", got)
	}
}

func TestGenerateDebianPkgbuild(t *testing.T) {
	rel := Release{
		ArchPackageName:        "vendor-app-bin",
		DisplayName:            "Vendor App",
		OriginalSourceFilename: "vendor app_1.2_amd64.deb",
		SourceSHA256:           strings.Repeat("a", 64),
		Debian: DebianMetadata{
			Version:      "2:1.2.3-4",
			Architecture: "amd64",
			Description:  "Vendor desktop application",
			Homepage:     "https://vendor.example",
		},
		Dependencies: []Dependency{{
			ArchPackage: "gtk3",
			Status:      MappingResolved,
		}},
		PayloadRules: []PayloadRule{{
			Path:     "etc/apt/sources.list.d/vendor.list",
			Excluded: true,
		}},
		Lifecycle: LifecycleScript{
			FileName:         "vendor-app-bin.install",
			Contents:         "post_install() {\n  :\n}\n",
			ValidationPassed: true,
		},
	}

	pkgbuild := Generate(rel)
	vars := IdentityVariables(rel)

	for _, snippet := range []string{
		`source "${startdir:-.}/pacsmith.vars"`,
		`pkgname="${_PACSMITH_PKGNAME}"`,
		`depends=('gtk3')`,
		`options=('!strip' '!debug')`,
		`source=("${_PACSMITH_SOURCE}")`,
		`sha256sums=("${_PACSMITH_SHA256}")`,
		`data.tar|data.tar.*`,
		`--no-same-owner`,
		`install="${_PACSMITH_INSTALL}"`,
		`${pkgdir}/etc/apt/sources.list.d/vendor.list`,
		`  rm -rf -- "${pkgdir}/etc/apt/sources.list.d/vendor.list"`,
	} {
		if !strings.Contains(pkgbuild, snippet) {
			t.Errorf("PKGBUILD missing %q\n%s", snippet, pkgbuild)
		}
	}
	if strings.Contains(pkgbuild, "postinst") {
		t.Error("PKGBUILD must not embed Debian postinst")
	}

	for _, snippet := range []string{
		"_PACSMITH_PKGNAME='vendor-app-bin'",
		"_PACSMITH_EPOCH='2'",
		"_PACSMITH_PKGVER='1.2.3'",
		"_PACSMITH_ARCH='x86_64'",
		"_PACSMITH_SOURCE='vendor app_1.2_amd64.deb'",
		"_PACSMITH_INSTALL='vendor-app-bin.install'",
	} {
		if !strings.Contains(vars, snippet) {
			t.Errorf("pacsmith.vars missing %q\n%s", snippet, vars)
		}
	}

	rel.Lifecycle.ValidationPassed = false
	blocked := Generate(rel)
	if !strings.Contains(blocked, `install="${_PACSMITH_INSTALL}"`) {
		t.Error("blocked lifecycle PKGBUILD still references install= from vars")
	}
	if strings.Contains(IdentityVariables(rel), "_PACSMITH_INSTALL='vendor-app-bin.install'") {
		t.Error("unvalidated lifecycle must not be assigned in pacsmith.vars")
	}

	rel.Lifecycle.ValidationPassed = true
	rel.InstallMapping.Icon = Icon{
		SourcePath: "usr/share/pixmaps/vendor.png",
		SHA256:     strings.Repeat("c", 64),
		Format:     "png",
		IconName:   "vendor-app",
	}
	iconBuild := Generate(rel)
	iconVars := IdentityVariables(rel)
	if IconSourceName(rel) != "pacsmith-icon.png" {
		t.Fatalf("IconSourceName = %q", IconSourceName(rel))
	}
	if !strings.Contains(iconVars, "_PACSMITH_ICON='pacsmith-icon.png'") {
		t.Fatalf("icon vars missing source name:\n%s", iconVars)
	}
	if !strings.Contains(iconBuild, `/usr/share/icons/hicolor/256x256/apps/vendor-app.png`) {
		t.Fatalf("PKGBUILD missing icon install:\n%s", iconBuild)
	}
}

func TestGenerateMultiSourcePkgbuilds(t *testing.T) {
	rel := Release{
		ProjectID:              "vendor-tool",
		ID:                     "2.1-aaaaaaaaaaaa",
		ArchPackageName:        "vendor-tool-bin",
		DisplayName:            "Vendor Tool",
		OriginalSourceFilename: "vendor-tool-2.1-linux-x86_64.tar.gz",
		SourceSHA256:           strings.Repeat("a", 64),
		Debian: DebianMetadata{
			Version:      "2.1",
			Architecture: "amd64",
		},
		Acquisition: Acquisition{
			Kind:              AcquisitionGitHubRelease,
			CanonicalIdentity: "github:vendor/tool",
		},
		SourceType: SourceArchive,
		InstallMapping: InstallMapping{
			ArchiveLayout:     ArchiveOptBundle,
			OptDirectory:      "vendor-tool",
			CommonPrefix:      "vendor-tool-2.1",
			StripCommonPrefix: true,
			Launchers: []Launcher{{
				Enabled:     true,
				SourcePath:  "vendor-tool-2.1/bin/tool",
				CommandName: "tool",
				Destination: "/usr/bin/tool",
			}},
			DesktopEntries: []DesktopEntry{{
				ID:          "vendor-tool",
				Enabled:     true,
				Destination: "/usr/share/applications/vendor-tool.desktop",
				Contents:    "[Desktop Entry]\nType=Application\nName=Vendor Tool\nExec=tool\nIcon=vendor-tool\n",
			}},
			Icon: Icon{
				ProjectPath: "files/integration/icon.svg",
				SHA256:      strings.Repeat("b", 64),
				Format:      "svg",
				IconName:    "vendor-tool",
			},
		},
	}

	archive := Generate(rel)
	archiveVars := IdentityVariables(rel)
	for _, snippet := range []string{
		`options=('!strip' '!debug')`,
		`pacsmith.schema=1`,
		`pacsmith.source=${_PACSMITH_SOURCE_IDENTITY}`,
		`$pkgdir/opt/${_PACSMITH_OPT}`,
		`--strip-components 1`,
		`../../opt/${_PACSMITH_OPT}/bin/tool`,
		`$pkgdir/usr/bin/tool`,
		`source+=("${_PACSMITH_ICON}")`,
		`/usr/share/applications/vendor-tool.desktop`,
		`/usr/share/icons/hicolor/scalable/apps/vendor-tool.svg`,
		`--no-same-owner`,
	} {
		if !strings.Contains(archive, snippet) {
			t.Errorf("archive PKGBUILD missing %q", snippet)
		}
	}
	if !strings.Contains(archiveVars, "_PACSMITH_SOURCE_IDENTITY='github%3Avendor%2Ftool'") {
		t.Fatalf("identity missing percent-encoded source: %s", archiveVars)
	}
	if got := InstalledPayloadPath(rel, "vendor-tool-2.1/bin/tool"); got != "/opt/vendor-tool/bin/tool" {
		t.Fatalf("InstalledPayloadPath = %q", got)
	}

	rel.SourceType = SourceAppImage
	rel.OriginalSourceFilename = "VendorTool.AppImage"
	rel.InstallMapping.AppImageOffset = 4096
	rel.InstallMapping.Launchers[0].SourcePath = "AppRun"
	rel.InstallMapping.Launchers[0].Kind = LauncherWrapper
	rel.InstallMapping.DesktopEntries[0].SourcePath = "vendor-tool.desktop"
	rel.InstallMapping.DesktopEntries = append(rel.InstallMapping.DesktopEntries, DesktopEntry{
		ID:          "python3.10",
		Enabled:     false,
		SourcePath:  "usr/share/applications/python3.10.desktop",
		Destination: "/usr/share/applications/python3.10.desktop",
		Contents:    "[Desktop Entry]\nType=Application\nName=Python\nExec=python3.10\nNoDisplay=true\n",
	})
	rel.PayloadRules = []PayloadRule{{
		Path:     "etc",
		Excluded: true,
	}}

	appImage := Generate(rel)
	for _, snippet := range []string{
		`makedepends=('squashfs-tools')`,
		`Preserve the complete AppDir below /opt`,
		`unsquashfs -no-progress -no-xattrs -f -o ${_PACSMITH_APPIMAGE_OFFSET}`,
		`-type f -exec chmod u-s,g-s`,
		`APPDIR='/opt/${_PACSMITH_OPT}'`,
		`unset APPIMAGE`,
		`exec "/opt/${_PACSMITH_OPT}/AppRun" "\$@"`,
		`printf '%s'`,
	} {
		if !strings.Contains(appImage, snippet) {
			t.Errorf("AppImage PKGBUILD missing %q", snippet)
		}
	}
	for _, snippet := range []string{
		`exec -a`,
		`APPIMAGE=extracted`,
		`$pkgdir/opt/${_PACSMITH_OPT}/AppRun`,
		`printf '%%s'`,
		`$pkgdir/opt/vendor-tool/vendor-tool.desktop`,
		`python3.10.desktop`,
		`${pkgdir}/etc`,
	} {
		if strings.Contains(appImage, snippet) {
			t.Errorf("AppImage PKGBUILD must not contain %q", snippet)
		}
	}

	rel.InstallMapping.AppRun = AppRun{
		Script:           true,
		OriginalContents: "#!/bin/bash\nBINARY_NAME=$(basename \"$0\")\nexec \"$HERE/$BINARY_NAME\" \"$@\"\n",
		Contents:         "#!/bin/sh\nexec \"$APPDIR/vendor-tool\" \"$@\"\n",
		UserModified:     true,
	}
	overlaid := Generate(rel)
	for _, snippet := range []string{
		`$pkgdir/opt/${_PACSMITH_OPT}/AppRun`,
		`exec "$APPDIR/vendor-tool" "$@"`,
		`unset APPIMAGE`,
		`exec "/opt/${_PACSMITH_OPT}/AppRun" "\$@"`,
	} {
		if !strings.Contains(overlaid, snippet) {
			t.Errorf("overlaid AppImage PKGBUILD missing %q", snippet)
		}
	}

	rel.SourceType = SourceELF
	rel.OriginalSourceFilename = "tool"
	rel.InstallMapping.BinaryDestination = "/usr/bin/tool"
	rel.InstallMapping.Launchers = nil
	elf := Generate(rel)
	wantELF := `install -Dm755 "$srcdir/${_PACSMITH_SOURCE}" "$pkgdir/usr/bin/tool"`
	if !strings.Contains(elf, wantELF) {
		t.Errorf("ELF PKGBUILD missing %q\n%s", wantELF, elf)
	}

	rel.SourceType = SourceArchPackage
	rel.OriginalSourceFilename = "vendor-tool-2.1-1-x86_64.pkg.tar.zst"
	rel.ArchPkgrelOverride = "1.1"
	archPackage := Generate(rel)
	if !strings.Contains(archPackage, `pkgrel="${_PACSMITH_PKGREL}"`) {
		t.Error("arch package PKGBUILD missing pkgrel identity")
	}
	if !strings.Contains(IdentityVariables(rel), "_PACSMITH_PKGREL='1.1'") {
		t.Error("arch package identity missing pkgrel override")
	}
	for _, snippet := range []string{
		`--exclude './.PKGINFO'`,
		`--exclude './.INSTALL'`,
		`--no-same-owner`,
	} {
		if !strings.Contains(archPackage, snippet) {
			t.Errorf("arch package PKGBUILD missing %q", snippet)
		}
	}

	rel.SourceType = SourceRPM
	rpm := Generate(rel)
	if !strings.Contains(rpm, `bsdtar -xpf "$srcdir/${_PACSMITH_SOURCE}" --no-same-owner -C "$pkgdir"`) {
		t.Errorf("RPM PKGBUILD missing cpio extraction:\n%s", rpm)
	}
}

func TestQuoteUntrustedExclusionPaths(t *testing.T) {
	rel := Release{
		ArchPackageName: "vendor-bin",
		SourceType:      SourceDebian,
		PayloadRules: []PayloadRule{{
			Path:     `etc/apt/foo$(reboot)"`,
			Excluded: true,
		}},
	}
	pkgbuild := Generate(rel)
	if !strings.Contains(pkgbuild, `rm -rf -- "${pkgdir}/etc/apt/foo\$(reboot)\""`) {
		t.Fatalf("exclusion path was not double-quoted/escaped:\n%s", pkgbuild)
	}
}

func TestValidatePkgbuild(t *testing.T) {
	rel := Release{ArchPackageName: "tool-bin", SourceType: SourceDebian}
	got := Validate(Generate(rel))
	if !strings.Contains(got, "Basic structural validation passed") {
		t.Fatalf("Validate: %s", got)
	}
	stale := Validate("pkgname=tool\npkgver=1\npkgrel=1\narch=('any')\nsource=()\nsha256sums=()\npackage() {\n}\n")
	if !strings.Contains(stale, "identity variables") {
		t.Fatalf("expected stale-identity warning, got %s", stale)
	}
}

func TestValidateLifecycle(t *testing.T) {
	valid := "post_install() {\n  update-desktop-database -q\n}\npost_remove() {\n  update-desktop-database -q\n}\n"
	validation := ValidateLifecycle(valid)
	if !validation.Passed {
		t.Fatalf("valid lifecycle failed: %s", validation.Message())
	}

	unsafe := ValidateLifecycle("post_install() { curl https://vendor.example/key | apt-key add -; }\n")
	if unsafe.Passed {
		t.Fatal("unsafe lifecycle passed")
	}
	msg := unsafe.Message()
	if !strings.Contains(msg, "Network") || !strings.Contains(msg, "Package-manager") {
		t.Fatalf("unsafe message: %s", msg)
	}

	if got := ValidateLifecycle("   "); got.Passed || !strings.Contains(got.Message(), "empty") {
		t.Fatalf("empty: %+v", got)
	}
	if got := ValidateLifecycle("echo hi\n"); got.Passed || !strings.Contains(got.Message(), "No Arch lifecycle function") {
		t.Fatalf("no function: %s", got.Message())
	}
	if got := ValidateLifecycle("helper() { :; }\npost_install() { :; }\n"); got.Passed ||
		!strings.Contains(got.Message(), "Unsupported lifecycle function: helper") {
		t.Fatalf("unsupported: %s", got.Message())
	}

	marker := filepath.Join(t.TempDir(), "executed")
	sideEffect := "post_install() {\n  echo pwned > '" + marker + "'\n}\n"
	if got := ValidateLifecycle(sideEffect); !got.Passed {
		t.Fatalf("side-effect script failed syntax/policy: %s", got.Message())
	}
	if _, err := os.Stat(marker); err == nil {
		t.Fatal("lifecycle script was executed")
	}

	syntax := ValidateLifecycle("post_install() {\n  if\n}\n")
	if syntax.Passed {
		t.Fatal("syntax-invalid lifecycle passed")
	}
}

func TestGenerateCompatProvides(t *testing.T) {
	rel := Release{
		ArchPackageName:        "acme-slack",
		CompatPackageName:      "slack-desktop",
		Provides:               []string{"virtual-slack"},
		Conflicts:              []string{"old-slack"},
		OriginalSourceFilename: "slack.deb",
		SourceSHA256:           strings.Repeat("b", 64),
		Debian:                 DebianMetadata{Version: "1.0", Architecture: "amd64"},
	}
	pkgbuild := Generate(rel)
	vars := IdentityVariables(rel)
	for _, snippet := range []string{
		`provides=("${_PACSMITH_PROVIDES[@]}")`,
		`conflicts=("${_PACSMITH_CONFLICTS[@]}")`,
		`provides+=("${_PACSMITH_COMPAT_PKGNAME}=${pkgver}")`,
		`conflicts+=("${_PACSMITH_COMPAT_PKGNAME}")`,
	} {
		if !strings.Contains(pkgbuild, snippet) {
			t.Fatalf("PKGBUILD missing %q\n%s", snippet, pkgbuild)
		}
	}
	if !strings.Contains(vars, "_PACSMITH_COMPAT_PKGNAME='slack-desktop'") {
		t.Fatalf("vars missing compat name\n%s", vars)
	}
	if !strings.Contains(vars, "_PACSMITH_PROVIDES=('virtual-slack')") {
		t.Fatalf("vars missing explicit provides\n%s", vars)
	}
}
