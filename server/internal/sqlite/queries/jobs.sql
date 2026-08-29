-- name: InsertJob :one
INSERT INTO jobs (
    id, kind, status, project_id, release_id, payload_json, created_at
) VALUES (?, ?, ?, ?, ?, ?, ?)
RETURNING id, kind, status, project_id, release_id, payload_json, error, log_offset,
          message, current, total, failed_items, paused_items, created_at, started_at, finished_at;

-- name: GetJob :one
SELECT id, kind, status, project_id, release_id, payload_json, error, log_offset,
       message, current, total, failed_items, paused_items, created_at, started_at, finished_at
FROM jobs
WHERE id = ?;

-- name: UpdateJob :one
UPDATE jobs
SET status = ?,
    error = ?,
    log_offset = ?,
    started_at = ?,
    finished_at = ?,
    project_id = ?,
    release_id = ?,
    message = ?,
    current = ?,
    total = ?,
    failed_items = ?,
    paused_items = ?
WHERE id = ?
RETURNING id, kind, status, project_id, release_id, payload_json, error, log_offset,
          message, current, total, failed_items, paused_items, created_at, started_at, finished_at;

-- name: InterruptRunningJobs :exec
UPDATE jobs
SET status = 'interrupted',
    error = 'interrupted by daemon restart',
    finished_at = ?
WHERE status = 'running';

-- name: ListActiveJobsByKind :many
SELECT id, kind, status, project_id, release_id, payload_json, error, log_offset,
       message, current, total, failed_items, paused_items, created_at, started_at, finished_at
FROM jobs
WHERE kind = ? AND status IN ('queued', 'running')
ORDER BY created_at;

-- name: GetLatestLibraryJobCreatedAt :one
SELECT created_at
FROM jobs
WHERE kind = ?
  AND COALESCE(json_extract(payload_json, '$.release_id'), '') = ''
ORDER BY created_at DESC
LIMIT 1;

-- name: CountJobsByKind :one
SELECT COUNT(*)
FROM jobs
WHERE kind = ?;
