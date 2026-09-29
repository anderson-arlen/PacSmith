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

-- name: ListQueuedJobs :many
SELECT * FROM jobs WHERE status = 'queued' ORDER BY created_at, id;

-- name: ListRunningJobs :many
SELECT * FROM jobs WHERE status = 'running' ORDER BY created_at, id;

-- name: GetLatestBuildJobForRelease :one
SELECT * FROM jobs WHERE kind = 'build' AND release_id = ?
ORDER BY created_at DESC, id DESC LIMIT 1;

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

-- name: CountActiveStorageJobs :one
SELECT count(*) FROM jobs WHERE status IN ('queued', 'running')
AND (release_id = sqlc.arg(release_id) OR project_id = sqlc.arg(project_id)
     OR json_extract(payload_json, '$.release_id') = sqlc.arg(release_id)
     OR json_extract(payload_json, '$.existing_project_id') = sqlc.arg(project_id));
