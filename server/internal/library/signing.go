package library

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
	"github.com/anderson-arlen/pacsmith/server/internal/pgp"
)

type storedSigningKey struct {
	RelativePath      string
	SHA256            string
	Fingerprints      []string
	SourcePath        string
	SourceFingerprint string
	Trusted           bool
	ArtifactID        string
	Contents          []byte
	Provenance        inspect.FieldProvenance
}

func signingKeyJSON(key storedSigningKey) map[string]any {
	out := map[string]any{
		"relativePath":      key.RelativePath,
		"sha256":            key.SHA256,
		"fingerprints":      nonNilStrings(key.Fingerprints),
		"sourcePath":        key.SourcePath,
		"sourceFingerprint": key.SourceFingerprint,
		"trusted":           key.Trusted,
		"artifactId":        key.ArtifactID,
		"provenance":        provenanceJSON(key.Provenance),
	}
	if len(key.Contents) > 0 && key.ArtifactID == "" {
		out["contents"] = base64.StdEncoding.EncodeToString(key.Contents)
	}
	return out
}

func prepareSigningKey(contents []byte, sourcePath, sourceFingerprint string, origin inspect.ValueOrigin) (storedSigningKey, error) {
	normalized, err := pgp.Normalize(contents)
	if err != nil {
		return storedSigningKey{}, err
	}
	fingerprints, err := pgp.Fingerprints(normalized)
	if err != nil {
		return storedSigningKey{}, err
	}
	sum := sha256.Sum256(normalized)
	digest := hex.EncodeToString(sum[:])
	rationale := "Signing key was embedded in the imported vendor package."
	approved := false
	if origin == inspect.OriginUser {
		rationale = "Signing key was explicitly imported and trusted by the user."
		approved = true
	}
	if sourceFingerprint == "" {
		sourceFingerprint = digest
	}
	return storedSigningKey{
		RelativePath:      "files/keys/vendor-" + digest[:16] + ".gpg",
		SHA256:            digest,
		Fingerprints:      fingerprints,
		SourcePath:        sourcePath,
		SourceFingerprint: sourceFingerprint,
		Trusted:           true,
		Contents:          normalized,
		Provenance: inspect.FieldProvenance{
			Origin:            origin,
			SourceFingerprint: sourceFingerprint,
			Rationale:         rationale,
			Timestamp:         time.Now().UTC(),
			UserApproved:      approved,
		},
	}, nil
}

func signingKeysFromExtracted(keys []inspect.ExtractedSigningKey) []storedSigningKey {
	out := make([]storedSigningKey, 0, len(keys))
	seen := map[string]struct{}{}
	for _, key := range keys {
		prepared, err := prepareSigningKey(key.Contents, key.SourcePath, key.SourceFingerprint, inspect.OriginDeterministic)
		if err != nil {
			continue
		}
		if _, exists := seen[prepared.SHA256]; exists {
			continue
		}
		seen[prepared.SHA256] = struct{}{}
		out = append(out, prepared)
	}
	return out
}

func attachSigningKeys(document map[string]any) {
	if document == nil {
		return
	}
	update, ok := mapValue(document, "update")
	if !ok {
		update = map[string]any{"strategy": "Manual"}
		document["update"] = update
	}
	if signingKeysUsable(update["signingKeys"]) {
		return
	}
	inspection := inspect.InspectScripts(maintainerScriptsFromDocument(document))
	prepared := signingKeysFromExtracted(inspection.SigningKeys)
	if len(prepared) == 0 {
		return
	}
	encoded := make([]map[string]any, 0, len(prepared))
	for _, key := range prepared {
		encoded = append(encoded, signingKeyJSON(key))
	}
	update["signingKeys"] = encoded
	if stringValue(update, "aptSigningKeyring") == "" {
		update["aptSigningKeyring"] = prepared[0].RelativePath
	}
	if stringValue(update, "trustedSigningFingerprint") == "" && len(prepared[0].Fingerprints) > 0 {
		update["trustedSigningFingerprint"] = prepared[0].Fingerprints[0]
	}
}

func signingKeysUsable(raw any) bool {
	for _, key := range objectSlice(raw) {
		if stringValue(key, "artifactId") != "" {
			return true
		}
		if strings.TrimSpace(stringValue(key, "contents")) != "" {
			return true
		}
		provenance, _ := mapValue(key, "provenance")
		if stringValue(provenance, "origin") == "user" {
			return true
		}
	}
	return false
}

func storedSigningKeysFromDocument(update map[string]any) []storedSigningKey {
	var out []storedSigningKey
	for _, raw := range objectSlice(update["signingKeys"]) {
		key := storedSigningKey{
			RelativePath:      stringValue(raw, "relativePath"),
			SHA256:            stringValue(raw, "sha256"),
			Fingerprints:      stringSlice(raw["fingerprints"]),
			SourcePath:        stringValue(raw, "sourcePath"),
			SourceFingerprint: stringValue(raw, "sourceFingerprint"),
			Trusted:           boolValue(raw, "trusted"),
			ArtifactID:        firstNonEmpty(stringValue(raw, "artifactId"), stringValue(raw, "artifact_id")),
		}
		if encoded := strings.TrimSpace(stringValue(raw, "contents")); encoded != "" {
			if decoded, err := base64.StdEncoding.DecodeString(encoded); err == nil {
				key.Contents = decoded
			}
		}
		out = append(out, key)
	}
	return out
}
