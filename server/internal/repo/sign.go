package repo

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/pgp"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func (s *Service) InitSigning(ctx context.Context) (Settings, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return Settings{}, err
	}
	if row.SigningInitialized != 0 && row.SigningFingerprint != "" && row.KeyringVersion > 0 {
		return settingsFromRow(row), nil
	}
	if err := ensureDir(s.GnuPGHome); err != nil {
		return Settings{}, err
	}
	if row.SigningFingerprint == "" {
		if _, err := s.runGPG(ctx, nil, "--quick-generate-key",
			"PacSmith Repository Signing Key <noreply@pacsmith.local>",
			"ed25519", "default", "never"); err != nil {
			return Settings{}, fmt.Errorf("generate repository signing key: %w", err)
		}
		fingerprint, err := s.primaryFingerprint(ctx)
		if err != nil {
			return Settings{}, err
		}
		secretKey, err := s.runGPG(ctx, nil, "--export-secret-keys", "--armor", fingerprint)
		if err != nil {
			return Settings{}, fmt.Errorf("export repository signing key: %w", err)
		}
		if err := s.Secrets.Set(ctx, SecretSigningKey, secretKey); err != nil {
			return Settings{}, err
		}
		publicKey, err := s.runGPG(ctx, nil, "--export", "--armor", fingerprint)
		if err != nil {
			return Settings{}, err
		}
		pub, err := s.putBytes(ctx, "pacsmith.asc", "repo_pubkey", publicKey)
		if err != nil {
			return Settings{}, err
		}
		row.SigningInitialized = 1
		row.SigningFingerprint = fingerprint
		row.SigningPubkeyArtifactID = ns(pub.ID)
		row.ModifiedAt = s.nowString()
		updated, err := s.DB.Queries.UpdateRepoTrust(ctx, trustParams(row))
		if err != nil {
			return Settings{}, err
		}
		row = updated
	}
	if err := s.rebuildKeyringLocked(ctx, row); err != nil {
		return Settings{}, err
	}
	updated, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return Settings{}, err
	}
	return settingsFromRow(updated), nil
}

func (s *Service) PublicKey(ctx context.Context) ([]byte, string, error) {
	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return nil, "", err
	}
	if row.SigningPubkeyArtifactID.String == "" {
		return nil, "", fmt.Errorf("%w: repository signing is not initialized", ErrInvalid)
	}
	id := row.SigningPubkeyArtifactID.String
	if row.TrustMode == TrustRootCertified && row.CertifiedPubkeyArtifactID.Valid {
		id = row.CertifiedPubkeyArtifactID.String
	}
	record, file, err := s.Artifacts.Open(ctx, id)
	if err != nil {
		return nil, "", err
	}
	defer file.Close()
	body, err := io.ReadAll(file)
	if err != nil {
		return nil, "", err
	}
	return body, record.OriginalFilename, nil
}

func (s *Service) UploadRootKey(ctx context.Context, armored []byte) (Settings, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return Settings{}, err
	}
	if row.SigningInitialized == 0 {
		return Settings{}, fmt.Errorf("%w: initialize the PacSmith signing key first", ErrInvalid)
	}
	normalized, err := pgp.Normalize(armored)
	if err != nil {
		return Settings{}, fmt.Errorf("%w: %s", ErrInvalid, err.Error())
	}
	fps, err := pgp.Fingerprints(normalized)
	if err != nil {
		return Settings{}, fmt.Errorf("%w: %s", ErrInvalid, err.Error())
	}
	if len(fps) == 0 {
		return Settings{}, fmt.Errorf("%w: root public key has no fingerprints", ErrInvalid)
	}
	record, err := s.putBytes(ctx, "pacsmith-root.gpg", "repo_root_pubkey", armored)
	if err != nil {
		return Settings{}, err
	}
	row.RootPubkeyArtifactID = ns(record.ID)
	row.RootFingerprint = fps[0]
	row.ModifiedAt = s.nowString()
	updated, err := s.DB.Queries.UpdateRepoTrust(ctx, trustParams(row))
	if err != nil {
		return Settings{}, err
	}
	return settingsFromRow(updated), nil
}

func (s *Service) UploadCertifiedKey(ctx context.Context, armored []byte) (Settings, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return Settings{}, err
	}
	if row.SigningInitialized == 0 {
		return Settings{}, fmt.Errorf("%w: initialize the PacSmith signing key first", ErrInvalid)
	}
	if !row.RootPubkeyArtifactID.Valid {
		return Settings{}, fmt.Errorf("%w: upload the administrator root public key first", ErrInvalid)
	}
	if err := s.verifyCertifiedKey(ctx, row, armored); err != nil {
		return Settings{}, err
	}
	record, err := s.putBytes(ctx, "pacsmith-certified.asc", "repo_certified_pubkey", armored)
	if err != nil {
		return Settings{}, err
	}
	row.CertifiedPubkeyArtifactID = ns(record.ID)
	row.TrustMode = TrustRootCertified
	row.ModifiedAt = s.nowString()
	updated, err := s.DB.Queries.UpdateRepoTrust(ctx, trustParams(row))
	if err != nil {
		return Settings{}, err
	}
	if err := s.rebuildKeyringLocked(ctx, updated); err != nil {
		return Settings{}, err
	}
	updated, err = s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return Settings{}, err
	}
	return settingsFromRow(updated), nil
}

func (s *Service) verifyCertifiedKey(ctx context.Context, row sqlcdb.RepoSetting, armored []byte) error {
	expected := NormalizeFingerprint(row.SigningFingerprint)
	fps, err := pgp.Fingerprints(armored)
	if err != nil {
		return fmt.Errorf("%w: %s", ErrInvalid, err.Error())
	}
	matched := false
	for _, fp := range fps {
		if NormalizeFingerprint(fp) == expected {
			matched = true
			break
		}
	}
	if !matched {
		return fmt.Errorf("%w: certified key does not match the PacSmith signing key", ErrInvalid)
	}
	root, file, err := s.Artifacts.Open(ctx, row.RootPubkeyArtifactID.String)
	if err != nil {
		return err
	}
	rootBytes := make([]byte, root.SizeBytes)
	n, err := file.Read(rootBytes)
	_ = file.Close()
	if err != nil && n == 0 {
		return err
	}
	rootBytes = rootBytes[:n]
	tmp, err := os.MkdirTemp(s.WorkDir, "gpg-verify-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	_ = os.Chmod(tmp, 0o700)
	if _, err := runGPGHome(ctx, tmp, rootBytes, "--import"); err != nil {
		return fmt.Errorf("%w: root public key could not be imported", ErrInvalid)
	}
	if _, err := runGPGHome(ctx, tmp, armored, "--import"); err != nil {
		return fmt.Errorf("%w: certified PacSmith public key could not be imported", ErrInvalid)
	}
	out, err := runGPGHome(ctx, tmp, nil, "--check-sigs", "--with-colons", expected)
	if err != nil {
		return fmt.Errorf("%w: could not check certifications", ErrInvalid)
	}
	rootFP := NormalizeFingerprint(row.RootFingerprint)
	rootIDs := map[string]struct{}{rootFP: {}, strings.ToUpper(rootFP[len(rootFP)-16:]): {}}
	if extra, err := pgp.Fingerprints(rootBytes); err == nil {
		for _, fp := range extra {
			fp = NormalizeFingerprint(fp)
			rootIDs[fp] = struct{}{}
			if len(fp) >= 16 {
				rootIDs[fp[len(fp)-16:]] = struct{}{}
			}
		}
	}
	for _, line := range strings.Split(string(out), "\n") {
		fields := strings.Split(line, ":")
		if len(fields) < 5 || fields[0] != "sig" {
			continue
		}
		if fields[1] != "!" {
			continue
		}
		signer := strings.ToUpper(fields[4])
		issuer := ""
		if len(fields) > 16 {
			issuer = NormalizeFingerprint(fields[16])
		}
		if _, ok := rootIDs[signer]; ok {
			return nil
		}
		if issuer != "" {
			if _, ok := rootIDs[issuer]; ok {
				return nil
			}
		}
	}
	return fmt.Errorf("%w: certification is not from the uploaded root public key", ErrInvalid)
}

func (s *Service) signFile(ctx context.Context, path string) (string, error) {
	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return "", err
	}
	if row.SigningInitialized == 0 || row.SigningFingerprint == "" {
		return "", fmt.Errorf("%w: repository signing is not initialized", ErrInvalid)
	}
	if err := s.ensureSigningHome(ctx); err != nil {
		return "", err
	}
	if _, err := s.runGPG(ctx, nil, "--detach-sign", "--yes", "--local-user", row.SigningFingerprint, path); err != nil {
		return "", fmt.Errorf("sign %s: %w", filepath.Base(path), err)
	}
	return path + ".sig", nil
}

func (s *Service) ensureSigningHome(ctx context.Context) error {
	if err := ensureDir(s.GnuPGHome); err != nil {
		return err
	}
	secretKey, err := s.Secrets.Get(ctx, SecretSigningKey)
	if err != nil {
		return fmt.Errorf("%w: repository signing key is unavailable", ErrInvalid)
	}
	if _, err := s.runGPG(ctx, secretKey, "--import"); err != nil {
		return fmt.Errorf("import repository signing key: %w", err)
	}
	return nil
}

func (s *Service) primaryFingerprint(ctx context.Context) (string, error) {
	out, err := s.runGPG(ctx, nil, "--list-secret-keys", "--with-colons")
	if err != nil {
		return "", err
	}
	sawPub := false
	for _, line := range strings.Split(string(out), "\n") {
		fields := strings.Split(line, ":")
		if len(fields) < 10 {
			continue
		}
		if fields[0] == "sec" || fields[0] == "pub" {
			sawPub = true
			continue
		}
		if sawPub && fields[0] == "fpr" {
			return NormalizeFingerprint(fields[9]), nil
		}
	}
	return "", fmt.Errorf("could not read generated key fingerprint")
}

func (s *Service) runGPG(ctx context.Context, stdin []byte, args ...string) ([]byte, error) {
	return runGPGHome(ctx, s.GnuPGHome, stdin, args...)
}

func runGPGHome(ctx context.Context, home string, stdin []byte, args ...string) ([]byte, error) {
	all := append([]string{
		"--homedir", home,
		"--batch",
		"--yes",
		"--pinentry-mode", "loopback",
		"--passphrase", "",
	}, args...)
	cmd := exec.CommandContext(ctx, "/usr/bin/gpg", all...)
	cmd.Env = append(os.Environ(), "GNUPGHOME="+home)
	if stdin != nil {
		cmd.Stdin = bytes.NewReader(stdin)
	}
	out, err := cmd.CombinedOutput()
	if err != nil {
		msg := strings.TrimSpace(string(out))
		if msg == "" {
			return nil, err
		}
		return nil, fmt.Errorf("%s", msg)
	}
	return out, nil
}

func trustParams(row sqlcdb.RepoSetting) sqlcdb.UpdateRepoTrustParams {
	return sqlcdb.UpdateRepoTrustParams{
		TrustMode:                   row.TrustMode,
		SigningFingerprint:          row.SigningFingerprint,
		SigningInitialized:          row.SigningInitialized,
		SigningPubkeyArtifactID:     row.SigningPubkeyArtifactID,
		RootPubkeyArtifactID:        row.RootPubkeyArtifactID,
		RootFingerprint:             row.RootFingerprint,
		CertifiedPubkeyArtifactID:   row.CertifiedPubkeyArtifactID,
		KeyringGpgArtifactID:        row.KeyringGpgArtifactID,
		KeyringTrustedArtifactID:    row.KeyringTrustedArtifactID,
		KeyringRevokedArtifactID:    row.KeyringRevokedArtifactID,
		KeyringPackageArtifactID:    row.KeyringPackageArtifactID,
		KeyringPackageSigArtifactID: row.KeyringPackageSigArtifactID,
		KeyringVersion:              row.KeyringVersion,
		RecoveryMessage:             row.RecoveryMessage,
		ModifiedAt:                  row.ModifiedAt,
	}
}

func (s *Service) putBytes(ctx context.Context, name, kind string, body []byte) (struct{ ID string }, error) {
	record, err := s.Artifacts.Put(ctx, name, kind, bytes.NewReader(body))
	if err != nil {
		return struct{ ID string }{}, err
	}
	return struct{ ID string }{ID: record.ID}, nil
}
