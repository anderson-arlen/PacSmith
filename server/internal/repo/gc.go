package repo

import (
	"context"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func (s *Service) ProtectedArtifactIDs(ctx context.Context) (map[string]struct{}, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.protectedLocked(ctx)
}

func (s *Service) protectedLocked(ctx context.Context) (map[string]struct{}, error) {
	roots := map[string]struct{}{}
	add := func(id string) {
		if id != "" {
			roots[id] = struct{}{}
		}
	}
	entries, err := s.DB.Queries.ListChannelEntries(ctx)
	if err != nil {
		return nil, err
	}
	for _, entry := range entries {
		add(entry.ArtifactID)
		add(entry.SigArtifactID.String)
	}
	soaks, err := s.DB.Queries.ListActiveSoaks(ctx)
	if err != nil {
		return nil, err
	}
	for _, soak := range soaks {
		add(soak.ArtifactID)
		add(soak.SigArtifactID.String)
	}
	dbs, err := s.DB.Queries.ListRepoDatabases(ctx)
	if err != nil {
		return nil, err
	}
	for _, db := range dbs {
		add(db.DbArtifactID)
		add(db.DbSigArtifactID.String)
		add(db.FilesArtifactID.String)
		add(db.FilesSigArtifactID.String)
	}
	settings, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return nil, err
	}
	addIDs(roots, settings)
	return roots, nil
}

func addIDs(roots map[string]struct{}, settings sqlcdb.RepoSetting) {
	for _, id := range []string{
		settings.SigningPubkeyArtifactID.String,
		settings.RootPubkeyArtifactID.String,
		settings.CertifiedPubkeyArtifactID.String,
		settings.KeyringGpgArtifactID.String,
		settings.KeyringTrustedArtifactID.String,
		settings.KeyringRevokedArtifactID.String,
		settings.KeyringPackageArtifactID.String,
		settings.KeyringPackageSigArtifactID.String,
	} {
		if id != "" {
			roots[id] = struct{}{}
		}
	}
}
