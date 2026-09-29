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

-- name: DeleteUnreferencedArtifact :execrows
DELETE FROM artifacts WHERE id = ?
AND NOT EXISTS (SELECT 1 FROM releases WHERE artifacts.id IN (source_artifact_id))
AND NOT EXISTS (SELECT 1 FROM projects WHERE artifacts.id IN (icon_artifact_id))
AND NOT EXISTS (SELECT 1 FROM release_artifacts WHERE artifacts.id IN (artifact_id))
AND NOT EXISTS (SELECT 1 FROM build_artifacts WHERE artifacts.id IN (artifact_id))
AND NOT EXISTS (SELECT 1 FROM repo_channel_entries WHERE artifacts.id IN (artifact_id, sig_artifact_id))
AND NOT EXISTS (SELECT 1 FROM repo_soaks WHERE artifacts.id IN (artifact_id, sig_artifact_id))
AND NOT EXISTS (SELECT 1 FROM repo_databases WHERE artifacts.id IN (db_artifact_id, db_sig_artifact_id, files_artifact_id, files_sig_artifact_id))
AND NOT EXISTS (SELECT 1 FROM repo_settings WHERE artifacts.id IN (signing_pubkey_artifact_id, root_pubkey_artifact_id, certified_pubkey_artifact_id, keyring_gpg_artifact_id, keyring_trusted_artifact_id, keyring_revoked_artifact_id, keyring_package_artifact_id, keyring_package_sig_artifact_id))
AND NOT EXISTS (SELECT 1 FROM jobs WHERE status IN ('queued', 'running')
                AND json_extract(payload_json, '$.artifact_id') = artifacts.id);

-- name: ListReleaseStorageArtifactIDs :many
SELECT source_artifact_id AS artifact_id FROM releases
WHERE releases.id = sqlc.arg(release_id) AND source_artifact_id IS NOT NULL
UNION SELECT artifact_id FROM release_artifacts WHERE release_id = sqlc.arg(release_id)
UNION SELECT artifact_id FROM build_artifacts
JOIN builds ON builds.id = build_artifacts.build_id
WHERE builds.release_id = sqlc.arg(release_id);

-- name: ListUnreferencedArtifacts :many
SELECT * FROM artifacts
WHERE NOT EXISTS (SELECT 1 FROM releases WHERE artifacts.id IN (source_artifact_id))
AND NOT EXISTS (SELECT 1 FROM projects WHERE artifacts.id IN (icon_artifact_id))
AND NOT EXISTS (SELECT 1 FROM release_artifacts WHERE artifacts.id IN (artifact_id))
AND NOT EXISTS (SELECT 1 FROM build_artifacts WHERE artifacts.id IN (artifact_id))
AND NOT EXISTS (SELECT 1 FROM repo_channel_entries WHERE artifacts.id IN (artifact_id, sig_artifact_id))
AND NOT EXISTS (SELECT 1 FROM repo_soaks WHERE artifacts.id IN (artifact_id, sig_artifact_id))
AND NOT EXISTS (SELECT 1 FROM repo_databases WHERE artifacts.id IN (db_artifact_id, db_sig_artifact_id, files_artifact_id, files_sig_artifact_id))
AND NOT EXISTS (SELECT 1 FROM repo_settings WHERE artifacts.id IN (signing_pubkey_artifact_id, root_pubkey_artifact_id, certified_pubkey_artifact_id, keyring_gpg_artifact_id, keyring_trusted_artifact_id, keyring_revoked_artifact_id, keyring_package_artifact_id, keyring_package_sig_artifact_id))
AND NOT EXISTS (SELECT 1 FROM jobs WHERE status IN ('queued', 'running')
                AND json_extract(payload_json, '$.artifact_id') = artifacts.id);
