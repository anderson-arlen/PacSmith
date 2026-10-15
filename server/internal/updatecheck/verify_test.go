package updatecheck

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

func TestAlternateStoredSigningKeyExplainsPinnedKeyRotation(t *testing.T) {
	update := map[string]any{
		"trustedSigningFingerprint": "418A7F2FB0E1E6E7EABF6FE8C2E73424D59097AB",
		"signingKeys": []any{
			map[string]any{"trusted": true, "fingerprints": []any{
				"418A7F2FB0E1E6E7EABF6FE8C2E73424D59097AB"}},
			map[string]any{"trusted": true, "fingerprints": []any{
				"DB085A08CA13B8ACB917E0F6D938EC0D038651BD",
				"D537997ABE0E1079C0E8EBD6C6ABDCF64DB9A0B2"}},
		},
	}
	message := alternateStoredSigningKey(update,
		"gpgv: using RSA key D537997ABE0E1079C0E8EBD6C6ABDCF64DB9A0B2")
	if !strings.Contains(message, "DB085A08CA13B8ACB917E0F6D938EC0D038651BD") ||
		!strings.Contains(message, "D537997ABE0E1079C0E8EBD6C6ABDCF64DB9A0B2") ||
		!strings.Contains(message, "select the") {
		t.Fatalf("rotation message = %q", message)
	}
}

func TestRepositorySignaturesAllowOnlyMissingAdditionalKeys(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	home := filepath.Join(root, "gnupg")
	if err := os.Mkdir(home, 0o700); err != nil {
		t.Fatal(err)
	}
	gpg := func(arguments ...string) []byte {
		t.Helper()
		command := exec.Command("gpg", append([]string{"--homedir", home, "--batch",
			"--yes", "--pinentry-mode", "loopback", "--passphrase", ""}, arguments...)...)
		output, err := command.CombinedOutput()
		if err != nil {
			t.Fatalf("gpg %v: %v: %s", arguments, err, output)
		}
		return output
	}
	for _, uid := range []string{"first@pacsmith.invalid", "second@pacsmith.invalid"} {
		gpg("--quick-generate-key", uid, "ed25519", "sign", "1d")
	}
	var fingerprints []string
	for _, line := range strings.Split(string(gpg("--with-colons", "--list-keys")), "\n") {
		fields := strings.Split(line, ":")
		if len(fields) > 9 && fields[0] == "fpr" {
			fingerprints = append(fingerprints, fields[9])
		}
	}
	if len(fingerprints) != 2 {
		t.Fatalf("fingerprints = %v", fingerprints)
	}
	db, err := sqlite.Open(ctx, filepath.Join(root, "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	store, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
	if err != nil {
		t.Fatal(err)
	}
	registry := &artifact.Registry{DB: db, Store: store}
	service := &Service{Artifacts: registry}
	key, err := registry.Put(ctx, "pinned.gpg", "signing_key",
		bytes.NewReader(gpg("--export", fingerprints[0])))
	if err != nil {
		t.Fatal(err)
	}
	target := checkTarget{Update: map[string]any{
		"aptSigningKeyring":         "pinned.gpg",
		"trustedSigningFingerprint": fingerprints[0],
		"signingKeys": []any{map[string]any{"relativePath": "pinned.gpg",
			"trusted": true, "artifactId": key.ID}},
	}}
	contents := []byte("Origin: PacSmith test\nSuite: noble\n")
	message := filepath.Join(root, "Release")
	if err := os.WriteFile(message, contents, 0o600); err != nil {
		t.Fatal(err)
	}
	for _, detached := range []bool{false, true} {
		name := "clear-signed"
		action := "--clearsign"
		if detached {
			name, action = "detached", "--detach-sign"
		}
		t.Run(name, func(t *testing.T) {
			signedPath := filepath.Join(root, name)
			gpg("--local-user", fingerprints[0], "--local-user", fingerprints[1],
				"--armor", "--output", signedPath, action, message)
			signed, err := os.ReadFile(signedPath)
			if err != nil {
				t.Fatal(err)
			}
			verify := func(data []byte) error {
				if detached {
					return service.verifyDetached(ctx, target, data, signed)
				}
				return service.verifyClearSigned(ctx, target, data)
			}
			data := signed
			if detached {
				data = contents
			}
			if err := verify(data); err != nil {
				t.Fatalf("valid pinned signature with unknown extra signer: %v", err)
			}
			if err := verify(bytes.ReplaceAll(data, []byte("noble"), []byte("altered"))); err == nil {
				t.Fatal("accepted tampered metadata")
			}
			target.Update["trustedSigningFingerprint"] = fingerprints[1]
			if err := verify(data); err == nil {
				t.Fatal("accepted a missing pinned signer because another signer was valid")
			}
			target.Update["trustedSigningFingerprint"] = fingerprints[0]
		})
	}
	both, err := registry.Put(ctx, "both.gpg", "signing_key", bytes.NewReader(gpg("--export")))
	if err != nil {
		t.Fatal(err)
	}
	target.Update["signingKeys"] = []any{map[string]any{"relativePath": "pinned.gpg",
		"trusted": true, "artifactId": both.ID}}
	for _, pin := range fingerprints {
		target.Update["trustedSigningFingerprint"] = pin
		signed, err := os.ReadFile(filepath.Join(root, "clear-signed"))
		if err != nil {
			t.Fatal(err)
		}
		if err := service.verifyClearSigned(ctx, target, signed); err != nil {
			t.Fatalf("matching one of multiple valid signers (%s): %v", pin, err)
		}
	}
}

func TestMissingAdditionalKeysRejectOtherGPGFailures(t *testing.T) {
	unknown := "[GNUPG:] ERRSIG 089EBE08314DF160 1 10 01 1781281671 9\n" +
		"[GNUPG:] NO_PUBKEY 089EBE08314DF160\n"
	if !missingPublicKeysOnly(unknown) {
		t.Fatal("missing extra key was rejected")
	}
	for _, failure := range []string{
		"[GNUPG:] BADSIG 089EBE08314DF160 test",
		"[GNUPG:] FAILURE verify 123",
		"[GNUPG:] NODATA 1",
		"[GNUPG:] ERROR verify 123",
		"[GNUPG:] EXPKEYSIG 089EBE08314DF160 test",
	} {
		if missingPublicKeysOnly(unknown + failure) {
			t.Errorf("accepted additional failure: %s", failure)
		}
	}
	if missingPublicKeysOnly(strings.Replace(unknown, "1781281671 9", "1781281671 4", 1)) {
		t.Fatal("accepted a signature error other than a missing key")
	}
	if missingPublicKeysOnly(strings.Replace(unknown, "NO_PUBKEY 089EBE08314DF160", "NO_PUBKEY 1234567890ABCDEF", 1)) {
		t.Fatal("accepted unmatched signature error and missing key")
	}
}
