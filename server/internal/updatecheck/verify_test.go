package updatecheck

import (
	"strings"
	"testing"
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
