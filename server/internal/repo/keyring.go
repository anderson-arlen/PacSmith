package repo

import (
	"bytes"
	"context"
	"fmt"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

const keyringInstall = `post_install() {
  [ -x /usr/bin/pacman-key ] || return 0
  pacman-key --populate pacsmith
}

post_upgrade() {
  post_install
}
`

func (s *Service) rebuildKeyringLocked(ctx context.Context, row sqlcdb.RepoSetting) error {
	if row.SigningInitialized == 0 || row.SigningFingerprint == "" {
		return nil
	}
	if err := s.ensureSigningHome(ctx); err != nil {
		return err
	}
	exportIDs := []string{row.SigningFingerprint}
	trusted := NormalizeFingerprint(row.SigningFingerprint)
	ownerTrust := DirectKeyOwnerTrust
	if row.TrustMode == TrustRootCertified {
		if !row.RootPubkeyArtifactID.Valid || !row.CertifiedPubkeyArtifactID.Valid {
			return fmt.Errorf("%w: root-certified trust is missing key material", ErrInvalid)
		}
		if rec, file, err := s.Artifacts.Open(ctx, row.RootPubkeyArtifactID.String); err == nil {
			body := make([]byte, rec.SizeBytes)
			n, _ := file.Read(body)
			_ = file.Close()
			if _, err := s.runGPG(ctx, body[:n], "--import"); err != nil {
				return err
			}
		}
		if rec, file, err := s.Artifacts.Open(ctx, row.CertifiedPubkeyArtifactID.String); err == nil {
			body := make([]byte, rec.SizeBytes)
			n, _ := file.Read(body)
			_ = file.Close()
			if _, err := s.runGPG(ctx, body[:n], "--import"); err != nil {
				return err
			}
		}
		exportIDs = []string{row.RootFingerprint, row.SigningFingerprint}
		trusted = NormalizeFingerprint(row.RootFingerprint)
		// Full ownertrust lets this single root certification fully validate the operational key.
		ownerTrust = RootKeyOwnerTrust
	}
	gpgBytes, err := s.runGPG(ctx, nil, append([]string{"--export"}, exportIDs...)...)
	if err != nil {
		return fmt.Errorf("export keyring: %w", err)
	}
	trustedBody := []byte(trusted + ":" + ownerTrust + ":\n")
	revokedBody := []byte("")
	gpgRec, err := s.putBytes(ctx, "pacsmith.gpg", "repo_keyring", gpgBytes)
	if err != nil {
		return err
	}
	trustedRec, err := s.putBytes(ctx, "pacsmith-trusted", "repo_keyring_trusted", trustedBody)
	if err != nil {
		return err
	}
	revokedRec, err := s.putBytes(ctx, "pacsmith-revoked", "repo_keyring_revoked", revokedBody)
	if err != nil {
		return err
	}

	files := map[string][]byte{
		"usr/share/pacman/keyrings/pacsmith.gpg":     gpgBytes,
		"usr/share/pacman/keyrings/pacsmith-trusted": trustedBody,
		"usr/share/pacman/keyrings/pacsmith-revoked": revokedBody,
	}
	pkgverNum := row.KeyringVersion + 1
	if pkgverNum < 1 {
		pkgverNum = 1
	}
	pkgver := fmt.Sprintf("%d", pkgverNum)
	pkg, err := WritePackage(PackageInfo{
		Name:   KeyringPackage,
		Pkgver: pkgver,
		Pkgrel: "1",
		Arch:   "any",
	}, files, keyringInstall)
	if err != nil {
		return err
	}
	pkgRec, err := s.Artifacts.Put(ctx, KeyringPackage+"-"+pkgver+"-1-any.pkg.tar.zst", "arch_package", bytes.NewReader(pkg))
	if err != nil {
		return err
	}
	sigID, err := s.signArtifactLocked(ctx, pkgRec.ID, pkgRec.OriginalFilename)
	if err != nil {
		return err
	}
	if _, err := s.DB.Queries.GetRepoPackageByName(ctx, KeyringPackage); err != nil {
		if _, insErr := s.DB.Queries.InsertRepoPackage(ctx, sqlcdb.InsertRepoPackageParams{
			Pkgname:         KeyringPackage,
			OriginalPkgname: KeyringPackage,
			Internal:        1,
			CreatedAt:       s.nowString(),
		}); insErr != nil && !strings.Contains(insErr.Error(), "UNIQUE") {
			return insErr
		}
	}
	now := s.nowString()
	filename := KeyringPackage + "-" + pkgver + "-1-any.pkg.tar.zst"
	for _, channel := range []string{ChannelStable, ChannelUnstable} {
		if err := s.DB.Queries.UpsertChannelEntry(ctx, sqlcdb.UpsertChannelEntryParams{
			Channel:       channel,
			Arch:          "any",
			Pkgname:       KeyringPackage,
			Epoch:         0,
			Pkgver:        pkgver,
			Pkgrel:        "1",
			ArtifactID:    pkgRec.ID,
			SigArtifactID: ns(sigID),
			Filename:      filename,
			PublishedAt:   now,
		}); err != nil {
			return err
		}
	}
	row.KeyringGpgArtifactID = ns(gpgRec.ID)
	row.KeyringTrustedArtifactID = ns(trustedRec.ID)
	row.KeyringRevokedArtifactID = ns(revokedRec.ID)
	row.KeyringPackageArtifactID = ns(pkgRec.ID)
	row.KeyringPackageSigArtifactID = ns(sigID)
	row.KeyringVersion = pkgverNum
	row.ModifiedAt = now
	if _, err := s.DB.Queries.UpdateRepoTrust(ctx, trustParams(row)); err != nil {
		return err
	}
	return s.republishAllLocked(ctx)
}
