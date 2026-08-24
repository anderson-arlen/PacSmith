-- name: InsertArtifact :one
INSERT INTO artifacts (
    id, sha256, size_bytes, original_filename, kind, created_at
) VALUES (?, ?, ?, ?, ?, ?)
RETURNING id, sha256, size_bytes, original_filename, kind, created_at;

-- name: GetArtifact :one
SELECT id, sha256, size_bytes, original_filename, kind, created_at
FROM artifacts
WHERE id = ?;

-- name: GetArtifactBySHA256 :one
SELECT id, sha256, size_bytes, original_filename, kind, created_at
FROM artifacts
WHERE sha256 = ?;

-- name: ListArtifacts :many
SELECT id, sha256, size_bytes, original_filename, kind, created_at
FROM artifacts;

-- name: DeleteArtifact :exec
DELETE FROM artifacts WHERE id = ?;

-- name: ListSourceArtifactIDs :many
SELECT source_artifact_id FROM releases WHERE source_artifact_id IS NOT NULL;

-- name: ListProjectIconArtifactIDs :many
SELECT icon_artifact_id FROM projects WHERE icon_artifact_id IS NOT NULL;

-- name: ListAllReleaseArtifactIDs :many
SELECT artifact_id FROM release_artifacts;
