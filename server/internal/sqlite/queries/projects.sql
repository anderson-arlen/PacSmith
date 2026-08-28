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

-- name: DeleteProject :exec
DELETE FROM projects WHERE id = ?;
