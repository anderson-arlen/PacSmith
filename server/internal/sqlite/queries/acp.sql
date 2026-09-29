-- name: GetACPSettings :one
SELECT * FROM acp_settings WHERE id = 1;
-- name: UpdateACPSettings :one
UPDATE acp_settings SET profile_json = ?, revision = revision + 1 WHERE id = 1 AND revision = ? RETURNING *;
-- name: CreateACPConversation :one
INSERT INTO acp_conversations(id, project_id, release_id, automatic, title, profile_json, status, created_at, updated_at)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING *;
-- name: GetACPConversation :one
SELECT * FROM acp_conversations WHERE id = ?;
-- name: ListACPConversations :many
SELECT * FROM acp_conversations WHERE project_id = ? ORDER BY created_at DESC LIMIT 100;
-- name: ListActiveACPConversations :many
SELECT * FROM acp_conversations WHERE status IN ('starting', 'running', 'waiting');
-- name: UpdateACPConversation :exec
UPDATE acp_conversations SET session_id = ?, status = ?, config_json = ?, error = ?, updated_at = ? WHERE id = ?;
-- name: SetACPTitle :exec
UPDATE acp_conversations SET title = ?, updated_at = ? WHERE id = ?;
-- name: AppendACPEvent :one
INSERT INTO acp_events(conversation_id, kind, body_json) VALUES (?, ?, ?) RETURNING *;
-- name: ListACPEvents :many
SELECT * FROM acp_events WHERE conversation_id = ? AND id > ? ORDER BY id LIMIT 500;
-- name: CreateACPPermission :one
INSERT INTO acp_permissions(id, conversation_id, request_json, created_at) VALUES (?, ?, ?, ?) RETURNING *;
-- name: GetACPPermission :one
SELECT * FROM acp_permissions WHERE id = ?;
-- name: ListACPPermissions :many
SELECT * FROM acp_permissions WHERE status = 'pending' ORDER BY created_at, id;
-- name: ResolveACPPermission :one
UPDATE acp_permissions SET status = ?, option_id = ?, answered_by = ?, resolved_at = ? WHERE id = ? AND status = 'pending' RETURNING *;
-- name: CancelACPPermissions :exec
UPDATE acp_permissions SET status = 'cancelled', resolved_at = ? WHERE conversation_id = ? AND status = 'pending';
-- name: RememberACPGrant :exec
INSERT OR IGNORE INTO acp_grants(identity, tool, session_id) VALUES (?, ?, ?);
-- name: HasACPGrant :one
SELECT COUNT(*) FROM acp_grants WHERE identity = ? AND tool = ? AND (session_id = '' OR session_id = ?);
-- name: ClearACPGrants :exec
DELETE FROM acp_grants WHERE identity = ?;
-- name: InvalidateACPPermissions :exec
UPDATE acp_permissions SET status = 'cancelled', resolved_at = ? WHERE status = 'pending';
-- name: ListACPReviewCandidates :many
SELECT r.id, r.project_id, p.display_name FROM releases r JOIN projects p ON p.id = r.project_id
WHERE p.auto_build_policy = 'ai'
AND json_extract(r.body_json, '$.update.lastAutomaticStatus') IN ('ai-pending', 'ai-reviewing')
AND COALESCE(json_extract(r.body_json, '$.buildStatus'), '') NOT IN ('building', 'succeeded');
