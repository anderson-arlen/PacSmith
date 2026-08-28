-- name: GetLibrarySettings :one
SELECT * FROM library_settings WHERE id = 1;

-- name: UpdateLibrarySettings :one
UPDATE library_settings
SET revision = revision + 1,
    ai_provider = ?,
    ai_model = ?,
    ai_reasoning_effort = ?,
    ai_execution_mode = ?,
    ai_auto_resolve = ?,
    updates_enabled = ?,
    updates_daily = ?,
    updates_weekday = ?,
    updates_hour = ?,
    updates_minute = ?,
    updates_auto_prepare = ?,
    retention_versions = ?,
    build_parallelism = ?
WHERE id = 1 AND revision = ?
RETURNING *;
