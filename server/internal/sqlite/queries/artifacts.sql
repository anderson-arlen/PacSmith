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
