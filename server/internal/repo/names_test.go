package repo

import (
	"strings"
	"testing"
)

func TestEffectiveNamePrefixAndOverride(t *testing.T) {
	got, original := EffectiveName("slack-desktop-bin", "slack-desktop", "pacsmith-", "")
	if got != "pacsmith-slack-desktop-bin" || original != "slack-desktop" {
		t.Fatalf("prefix: got %q original %q", got, original)
	}
	got, original = EffectiveName("slack-desktop-bin", "slack-desktop", "pacsmith-", "acme-slack")
	if got != "acme-slack" || original != "slack-desktop" {
		t.Fatalf("override: got %q original %q", got, original)
	}
	got, _ = EffectiveName("slack-desktop-bin", "slack-desktop", "", "")
	if got != "slack-desktop-bin" {
		t.Fatalf("no prefix: %q", got)
	}
}

func TestReservedNames(t *testing.T) {
	for _, name := range []string{"pacsmith", "pacsmithd", "pacsmith-gui", "pacsmith-keyring"} {
		if !IsReserved(name) {
			t.Fatalf("%s should be reserved", name)
		}
	}
	if IsReserved("slack-desktop") {
		t.Fatal("slack-desktop is not reserved")
	}
}

func TestCompatibilityAddsProvidesConflicts(t *testing.T) {
	provides, conflicts := Compatibility("pacsmith-slack-desktop-bin", "slack-desktop",
		[]string{"virtual-slack"}, []string{"old-slack"})
	if !containsExact(provides, "virtual-slack") || !containsExact(provides, "slack-desktop=${pkgver}") {
		t.Fatalf("provides %v", provides)
	}
	if !containsExact(conflicts, "old-slack") || !containsExact(conflicts, "slack-desktop") {
		t.Fatalf("conflicts %v", conflicts)
	}
	provides, conflicts = Compatibility("slack-desktop", "slack-desktop", []string{"keep"}, nil)
	if len(provides) != 1 || provides[0] != "keep" || len(conflicts) != 0 {
		t.Fatalf("same name mutated arrays: %v %v", provides, conflicts)
	}
}

func TestSanitizePrefix(t *testing.T) {
	got, err := SanitizePrefix("pacsmith")
	if err != nil || got != "pacsmith-" {
		t.Fatalf("got %q err %v", got, err)
	}
	got, err = SanitizePrefix("pacsmith-")
	if err != nil || got != "pacsmith-" {
		t.Fatalf("already dashed: %q", got)
	}
}

func TestKeyringPackageURL(t *testing.T) {
	settings := Settings{
		AdvertisedURL:  "https://packages.example.com/",
		KeyringVersion: 2,
	}
	if got := KeyringPackageFilename(2); got != "pacsmith-keyring-2-1-any.pkg.tar.zst" {
		t.Fatalf("filename %q", got)
	}
	want := "https://packages.example.com/repo/stable/any/pacsmith-keyring-2-1-any.pkg.tar.zst"
	if got := KeyringPackageURL(settings); got != want {
		t.Fatalf("url %q want %q", got, want)
	}
	if KeyringPackageURL(Settings{}) != "" {
		t.Fatal("empty keyring should have no url")
	}
}

func TestCertificationHelpUsesRealFingerprints(t *testing.T) {
	pacsmith := "ABCDEF0123456789ABCDEF0123456789ABCDEF01"
	root := "1234567890ABCDEF1234567890ABCDEF12345678"
	settings := Settings{Fingerprint: pacsmith, RootFingerprint: root}
	help := CertificationHelp(settings)
	commands := CertificationCommands(settings)
	for _, unexpected := range []string{"PACSMITH_FINGERPRINT", "ROOT_FINGERPRINT"} {
		if strings.Contains(help, unexpected) || strings.Contains(commands, unexpected) {
			t.Fatalf("placeholder %q still present\nhelp:\n%s\ncommands:\n%s", unexpected, help, commands)
		}
	}
	for _, want := range []string{pacsmith, root, "--quick-sign-key " + pacsmith, "--default-key " + root} {
		if !strings.Contains(help, want) || !strings.Contains(commands, want) {
			t.Fatalf("missing %q\nhelp:\n%s\ncommands:\n%s", want, help, commands)
		}
	}
}

func TestBootstrapFingerprintCheck(t *testing.T) {
	script := RenderBootstrap(Settings{
		ListenHosts:   []string{"127.0.0.1"},
		ListenPort:    8080,
		AdvertisedURL: "https://packages.example.com",
		Fingerprint:   "ABCDEF0123456789ABCDEF0123456789ABCDEF01",
		TrustMode:     TrustDirect,
	}, ChannelStable)
	for _, snippet := range []string{
		"EXPECTED_PACSMITH_FPR=",
		"EXPECTED_TRUSTED_FPR=",
		"EXPECTED_OWNER_TRUST=",
		"grep -qx \"$EXPECTED_PACSMITH_FPR\"",
		`printf '%s:%s:\n' "$EXPECTED_TRUSTED_FPR" "$EXPECTED_OWNER_TRUST"`,
		"SigLevel = Required TrustedOnly",
		`gpgdir=$(pacman-conf --config=/etc/pacman.conf gpgdir)`,
		`--import-ownertrust "$tmp/pacsmith-trusted"`,
		"pacman-key --updatedb",
		"${BASE_URL}/repo/${CHANNEL}/",
		"Keep repository access on a trusted private",
		"Tailscale, or WireGuard network",
		`BASE_URL="https://packages.example.com"`,
	} {
		if !strings.Contains(script, snippet) {
			t.Fatalf("bootstrap missing %q\n%s", snippet, script)
		}
	}
}

func containsExact(values []string, want string) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}
