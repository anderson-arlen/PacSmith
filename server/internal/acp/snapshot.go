package acp

import (
	"context"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

type ConversationSnapshot struct {
	Conversation sqlcdb.AcpConversation
	Events       []sqlcdb.AcpEvent
	Permissions  []sqlcdb.AcpPermission
}

func (s *Service) Snapshot(ctx context.Context, id string, after int64) (ConversationSnapshot, error) {
	var snapshot ConversationSnapshot
	tx, err := s.DB.SQL.BeginTx(ctx, nil)
	if err != nil {
		return snapshot, err
	}
	defer tx.Rollback()
	queries := s.DB.Queries.WithTx(tx)
	// Status and events must describe the same instant even if a turn finishes during this request.
	snapshot.Conversation, err = queries.GetACPConversation(ctx, id)
	if err != nil {
		return snapshot, err
	}
	snapshot.Events, err = queries.ListACPEvents(ctx, sqlcdb.ListACPEventsParams{ConversationID: id, ID: after})
	if err != nil {
		return snapshot, err
	}
	snapshot.Permissions, err = queries.ListACPPermissions(ctx)
	if err != nil {
		return snapshot, err
	}
	return snapshot, tx.Commit()
}

func (s *Service) completeOperation(ctx context.Context, row sqlcdb.AcpConversation, result map[string]any, operationErr error) error {
	tx, err := s.DB.SQL.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	queries := s.DB.Queries.WithTx(tx)
	if err := queries.UpdateACPConversation(ctx, sqlcdb.UpdateACPConversationParams{
		ID: row.ID, SessionID: row.SessionID, Status: row.Status, ConfigJson: row.ConfigJson,
		Error: row.Error, UpdatedAt: now(),
	}); err != nil {
		return err
	}
	if operationErr != nil || result != nil {
		kind, body := "finished", result
		if operationErr != nil {
			kind, body = "error", map[string]any{"message": operationErr.Error()}
		}
		if _, err := queries.AppendACPEvent(ctx, sqlcdb.AppendACPEventParams{
			ConversationID: row.ID, Kind: kind, BodyJson: encode(body),
		}); err != nil {
			return err
		}
	}
	if err := queries.CancelACPPermissions(ctx, sqlcdb.CancelACPPermissionsParams{
		ConversationID: row.ID, ResolvedAt: now(),
	}); err != nil {
		return err
	}
	return tx.Commit()
}
