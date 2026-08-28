package updatecheck

import (
	"context"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"time"
)

func (s *Service) verifyClearSigned(ctx context.Context, target checkTarget, contents []byte) error {
	return s.verifyGPG(ctx, target, contents, nil)
}

func (s *Service) verifyDetached(ctx context.Context, target checkTarget, contents, signature []byte) error {
	return s.verifyGPG(ctx, target, contents, signature)
}

func (s *Service) verifyGPG(ctx context.Context, target checkTarget, contents, signature []byte) error {
	keyring, fingerprints, err := s.signingKey(ctx, target)
	if err != nil {
		return err
	}
	contentsFile, err := os.CreateTemp("", "pacsmith-metadata-*")
	if err != nil {
		return err
	}
	contentsName := contentsFile.Name()
	defer os.Remove(contentsName)
	if err := contentsFile.Chmod(0o600); err != nil {
		contentsFile.Close()
		return err
	}
	if _, err := contentsFile.Write(contents); err != nil {
		contentsFile.Close()
		return err
	}
	if err := contentsFile.Close(); err != nil {
		return err
	}

	arguments := []string{"--status-fd", "1", "--keyring", keyring}
	var signatureName string
	if signature != nil {
		signatureFile, createErr := os.CreateTemp("", "pacsmith-signature-*")
		if createErr != nil {
			return createErr
		}
		signatureName = signatureFile.Name()
		defer os.Remove(signatureName)
		if chmodErr := signatureFile.Chmod(0o600); chmodErr != nil {
			signatureFile.Close()
			return chmodErr
		}
		if _, writeErr := signatureFile.Write(signature); writeErr != nil {
			signatureFile.Close()
			return writeErr
		}
		if closeErr := signatureFile.Close(); closeErr != nil {
			return closeErr
		}
		arguments = append(arguments, signatureName, contentsName)
	} else {
		arguments = append(arguments, contentsName)
	}

	commandContext, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	command := exec.CommandContext(commandContext, "gpgv", arguments...)
	output, err := command.CombinedOutput()
	if err != nil {
		detail := strings.TrimSpace(string(output))
		if suggestion := alternateStoredSigningKey(target.Update, detail); suggestion != "" {
			detail = suggestion + "\n" + detail
		}
		return fmt.Errorf("repository signature verification failed: %s", detail)
	}
	actual, primary := validSignatureFingerprints(string(output))
	if actual == "" {
		return fmt.Errorf("gpgv did not report a valid repository signature")
	}
	for _, pinned := range fingerprints {
		if strings.EqualFold(actual, pinned) || strings.EqualFold(primary, pinned) {
			return nil
		}
	}
	return fmt.Errorf("repository signature did not match the pinned signing-key fingerprint")
}

func alternateStoredSigningKey(update map[string]any, output string) string {
	pinned := strings.ToUpper(stringValue(update, "trustedSigningFingerprint"))
	upperOutput := strings.ToUpper(output)
	for _, key := range objects(update["signingKeys"]) {
		if !boolValue(key, "trusted") {
			continue
		}
		fingerprints := stringValues(key["fingerprints"])
		for _, fingerprint := range fingerprints {
			fingerprint = strings.ToUpper(fingerprint)
			if fingerprint == pinned || !validFingerprint(fingerprint) ||
				!strings.Contains(upperOutput, fingerprint) {
				continue
			}
			primary := fingerprint
			if len(fingerprints) > 0 && validFingerprint(strings.ToUpper(fingerprints[0])) {
				primary = strings.ToUpper(fingerprints[0])
			}
			return fmt.Sprintf("metadata is signed by stored key %s (signing fingerprint %s) instead of the pinned key; review the rotation and select the %s key in Update Monitoring if it is trusted", primary, fingerprint, primary)
		}
	}
	return ""
}

func validSignatureFingerprints(output string) (string, string) {
	for _, line := range strings.Split(output, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 3 || fields[0] != "[GNUPG:]" || fields[1] != "VALIDSIG" {
			continue
		}
		actual := strings.ToUpper(fields[2])
		if !validFingerprint(actual) {
			continue
		}
		primary := ""
		if len(fields) >= 12 {
			candidate := strings.ToUpper(fields[len(fields)-1])
			if validFingerprint(candidate) {
				primary = candidate
			}
		}
		return actual, primary
	}
	return "", ""
}

func validFingerprint(value string) bool {
	decoded, err := hex.DecodeString(value)
	return err == nil && (len(decoded) == 20 || len(decoded) == 32)
}

func (s *Service) signingKey(ctx context.Context, target checkTarget) (string, []string, error) {
	configured := stringValue(target.Update, "aptSigningKeyring")
	pinned := strings.ToUpper(stringValue(target.Update, "trustedSigningFingerprint"))
	if configured == "" || pinned == "" {
		return "", nil, fmt.Errorf("update checks require a trusted signing key and pinned fingerprint")
	}
	artifactID := ""
	for _, key := range objects(target.Update["signingKeys"]) {
		if stringValue(key, "relativePath") != configured || !boolValue(key, "trusted") {
			continue
		}
		artifactID = stringValue(key, "artifactId")
		if artifactID == "" {
			artifactID = stringValue(key, "artifact_id")
		}
		break
	}
	if artifactID == "" {
		return "", nil, fmt.Errorf("configured repository signing key has no server artifact")
	}
	record, err := s.Artifacts.Get(ctx, artifactID)
	if err != nil {
		return "", nil, fmt.Errorf("load repository signing key: %w", err)
	}
	path, err := s.Artifacts.Store.Path(record.SHA256)
	if err != nil {
		return "", nil, err
	}
	return path, []string{pinned}, nil
}
