package repo

import (
	"context"
	"os"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

const missingSigningKeyMessage = "Repository publishing was disabled because its private signing key was missing from the secret service. Previously published indexes were cleared. Initialize a new signing key, update client trust, then re-enable the repository."

func (s *Service) RecoverMissingSigningKey(ctx context.Context) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	row, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return false, err
	}
	if row.SigningInitialized == 0 {
		return false, nil
	}
	exists, err := s.Secrets.Exists(ctx, SecretSigningKey)
	if err != nil {
		return false, err
	}
	if exists {
		return false, nil
	}

	if err := os.RemoveAll(s.GnuPGHome); err != nil {
		return false, err
	}
	tx, err := s.DB.SQL.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	queries := s.DB.Queries.WithTx(tx)
	if err := queries.DeleteAllChannelEntries(ctx); err != nil {
		_ = tx.Rollback()
		return false, err
	}
	if err := queries.DeleteAllSoaks(ctx); err != nil {
		_ = tx.Rollback()
		return false, err
	}
	if err := queries.DeleteAllRepoDatabases(ctx); err != nil {
		_ = tx.Rollback()
		return false, err
	}
	if _, err := queries.ResetRepoAfterSigningKeyLoss(ctx, sqlcdb.ResetRepoAfterSigningKeyLossParams{
		RecoveryMessage: missingSigningKeyMessage,
		ModifiedAt:      s.nowString(),
	}); err != nil {
		_ = tx.Rollback()
		return false, err
	}
	if err := tx.Commit(); err != nil {
		return false, err
	}
	return true, nil
}
