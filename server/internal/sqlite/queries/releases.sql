-- name: InsertRelease :one
INSERT INTO releases (
    id, project_id, revision, state, source_type, vendor_version, original_filename,
    source_sha256, source_artifact_id, arch_package_name, arch_pkgrel, body_json,
    created_at, modified_at
) VALUES (?, ?, 1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
RETURNING *;

-- name: GetRelease :one
SELECT * FROM releases WHERE id = ?;

-- name: GetReleaseByProjectSHA256 :one
SELECT * FROM releases WHERE project_id = ? AND source_sha256 = ?;

-- name: ListReleasesForProject :many
SELECT * FROM releases WHERE project_id = ? ORDER BY created_at;

-- name: UpdateRelease :one
UPDATE releases
SET state = ?,
    source_type = ?,
    vendor_version = ?,
    original_filename = ?,
    source_sha256 = ?,
    source_artifact_id = ?,
    arch_package_name = ?,
    arch_pkgrel = ?,
    body_json = ?,
    modified_at = ?,
    revision = revision + 1
WHERE id = ? AND revision = ?
RETURNING *;

-- name: DeleteRelease :exec
DELETE FROM releases WHERE id = ?;

-- name: InsertUpdateSource :one
INSERT INTO update_sources (id, release_id, revision, strategy, config_json)
VALUES (?, ?, 1, ?, ?)
RETURNING *;

-- name: GetUpdateSourceByRelease :one
SELECT * FROM update_sources WHERE release_id = ?;

-- name: UpdateUpdateSource :one
UPDATE update_sources
SET strategy = ?,
    config_json = ?,
    revision = revision + 1
WHERE release_id = ? AND revision = ?
RETURNING *;

-- name: ListUpdateSources :many
SELECT * FROM update_sources;

-- name: UpsertUpdateCheckState :exec
INSERT INTO update_check_state (
    update_source_id, last_checked_at, last_message, last_error, detected_version,
    detected_filename, detected_sha256, detected_url, etag, signature_verified, job_id
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT (update_source_id) DO UPDATE SET
    last_checked_at = excluded.last_checked_at,
    last_message = excluded.last_message,
    last_error = excluded.last_error,
    detected_version = excluded.detected_version,
    detected_filename = excluded.detected_filename,
    detected_sha256 = excluded.detected_sha256,
    detected_url = excluded.detected_url,
    etag = excluded.etag,
    signature_verified = excluded.signature_verified,
    job_id = excluded.job_id;

-- name: GetUpdateCheckState :one
SELECT * FROM update_check_state WHERE update_source_id = ?;

-- name: InsertReleaseArtifact :exec
INSERT OR IGNORE INTO release_artifacts (release_id, artifact_id, role)
VALUES (?, ?, ?);

-- name: DeleteReleaseArtifactsByRole :exec
DELETE FROM release_artifacts WHERE release_id = ? AND role = ?;

-- name: ListReleaseArtifacts :many
SELECT artifact_id, role FROM release_artifacts WHERE release_id = ?;

-- name: InsertBuild :one
INSERT INTO builds (id, release_id, status, log_text, started_at, finished_at)
VALUES (?, ?, ?, ?, ?, ?)
RETURNING *;

-- name: ListBuildsForRelease :many
SELECT * FROM builds WHERE release_id = ? ORDER BY started_at;

-- name: InsertBuildArtifact :exec
INSERT OR IGNORE INTO build_artifacts (build_id, artifact_id)
VALUES (?, ?);

-- name: ListBuildArtifactsForBuild :many
SELECT artifacts.*
FROM build_artifacts
JOIN artifacts ON artifacts.id = build_artifacts.artifact_id
WHERE build_artifacts.build_id = ?
ORDER BY artifacts.created_at, artifacts.id;
