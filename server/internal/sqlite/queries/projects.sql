-- name: InsertProject :one
INSERT INTO projects (
    id, revision, display_name, arch_package_name, vendor_name, source_identity,
    icon_artifact_id, icon_sha256, history_json, created_at, modified_at
) VALUES (?, 1, ?, ?, ?, ?, ?, ?, ?, ?, ?)
RETURNING *;

-- name: GetProject :one
SELECT * FROM projects WHERE id = ?;

-- name: ListProjects :many
SELECT * FROM projects ORDER BY display_name COLLATE NOCASE, id;

-- name: ListProjectsBySourceIdentity :many
SELECT * FROM projects WHERE source_identity = ?;

-- name: ReplaceProjectHistory :one
UPDATE projects
SET history_json = sqlc.arg(history_json),
    modified_at = sqlc.arg(modified_at),
    revision = revision + 1
WHERE id = sqlc.arg(id) AND revision = sqlc.arg(revision)
RETURNING *;

-- name: UpdateProject :one
UPDATE projects
SET display_name = ?,
    arch_package_name = ?,
    vendor_name = ?,
    source_identity = ?,
    icon_artifact_id = ?,
    icon_sha256 = ?,
    history_json = ?,
    auto_build_policy = ?,
    compile_cache_policy = ?,
    modified_at = ?,
    revision = revision + 1
WHERE id = ? AND revision = ?
RETURNING *;

-- name: AppendProjectHistory :one
UPDATE projects
SET history_json = json_insert(history_json, '$[#]', json(sqlc.arg(entry_json))),
    modified_at = sqlc.arg(modified_at),
    revision = revision + 1
WHERE id = sqlc.arg(id)
  AND json_valid(history_json)
  AND json_type(history_json) = 'array'
RETURNING *;

-- name: DeleteProject :exec
DELETE FROM projects WHERE id = ?;
