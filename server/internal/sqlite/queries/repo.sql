-- name: GetRepoSettings :one
SELECT * FROM repo_settings WHERE id = 1;

-- name: UpdateRepoSettings :one
UPDATE repo_settings
SET revision = revision + 1,
    enabled = ?,
    listen_hosts = ?,
    listen_port = ?,
    advertised_url = ?,
    stable_enabled = ?,
    soak_seconds = ?,
    package_name_prefix = ?,
    trust_mode = ?,
    signing_fingerprint = ?,
    signing_initialized = ?,
    signing_pubkey_artifact_id = ?,
    root_pubkey_artifact_id = ?,
    root_fingerprint = ?,
    certified_pubkey_artifact_id = ?,
    keyring_gpg_artifact_id = ?,
    keyring_trusted_artifact_id = ?,
    keyring_revoked_artifact_id = ?,
    keyring_package_artifact_id = ?,
    keyring_package_sig_artifact_id = ?,
    keyring_version = ?,
    modified_at = ?
WHERE id = 1 AND revision = ?
RETURNING *;

-- name: UpdateRepoTrust :one
UPDATE repo_settings
SET revision = revision + 1,
    trust_mode = ?,
    signing_fingerprint = ?,
    signing_initialized = ?,
    signing_pubkey_artifact_id = ?,
    root_pubkey_artifact_id = ?,
    root_fingerprint = ?,
    certified_pubkey_artifact_id = ?,
    keyring_gpg_artifact_id = ?,
    keyring_trusted_artifact_id = ?,
    keyring_revoked_artifact_id = ?,
    keyring_package_artifact_id = ?,
    keyring_package_sig_artifact_id = ?,
    keyring_version = ?,
    modified_at = ?
WHERE id = 1
RETURNING *;

-- name: GetRepoPackageByName :one
SELECT * FROM repo_packages WHERE pkgname = ?;

-- name: GetRepoPackageByProject :one
SELECT * FROM repo_packages WHERE project_id = ?;

-- name: ListRepoPackages :many
SELECT * FROM repo_packages ORDER BY pkgname;

-- name: InsertRepoPackage :one
INSERT INTO repo_packages (pkgname, project_id, original_pkgname, internal, created_at)
VALUES (?, ?, ?, ?, ?)
RETURNING *;

-- name: DeleteRepoPackageByProject :exec
DELETE FROM repo_packages WHERE project_id = ?;

-- name: DeleteRepoPackageByName :exec
DELETE FROM repo_packages WHERE pkgname = ?;

-- name: GetChannelEntry :one
SELECT * FROM repo_channel_entries
WHERE channel = ? AND arch = ? AND pkgname = ?;

-- name: ListChannelEntries :many
SELECT * FROM repo_channel_entries
ORDER BY channel, arch, pkgname;

-- name: ListChannelEntriesForChannelArch :many
SELECT * FROM repo_channel_entries
WHERE channel = ? AND arch = ?
ORDER BY pkgname;

-- name: ListChannelArches :many
SELECT DISTINCT arch FROM repo_channel_entries ORDER BY arch;

-- name: UpsertChannelEntry :exec
INSERT INTO repo_channel_entries (
    channel, arch, pkgname, project_id, release_id, epoch, pkgver, pkgrel,
    artifact_id, sig_artifact_id, filename, published_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT (channel, arch, pkgname) DO UPDATE SET
    project_id = excluded.project_id,
    release_id = excluded.release_id,
    epoch = excluded.epoch,
    pkgver = excluded.pkgver,
    pkgrel = excluded.pkgrel,
    artifact_id = excluded.artifact_id,
    sig_artifact_id = excluded.sig_artifact_id,
    filename = excluded.filename,
    published_at = excluded.published_at;

-- name: DeleteChannelEntriesForProject :exec
DELETE FROM repo_channel_entries WHERE project_id = ?;

-- name: DeleteChannelEntry :exec
DELETE FROM repo_channel_entries WHERE channel = ? AND arch = ? AND pkgname = ?;

-- name: GetSoak :one
SELECT * FROM repo_soaks WHERE pkgname = ? AND arch = ? AND pkgver = ?;

-- name: ListSoaks :many
SELECT * FROM repo_soaks ORDER BY pkgname, arch, pkgver;

-- name: ListSoaksForPackage :many
SELECT * FROM repo_soaks WHERE pkgname = ? AND arch = ? ORDER BY pkgver;

-- name: ListActiveSoaks :many
SELECT * FROM repo_soaks WHERE status IN ('soaking', 'eligible') ORDER BY pkgname, arch, pkgver;

-- name: UpsertSoak :exec
INSERT INTO repo_soaks (
    pkgname, arch, pkgver, project_id, release_id, epoch, pkgrel, artifact_id,
    sig_artifact_id, soak_started_at, eligible_at, status
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT (pkgname, arch, pkgver) DO UPDATE SET
    project_id = excluded.project_id,
    release_id = excluded.release_id,
    epoch = excluded.epoch,
    pkgrel = excluded.pkgrel,
    artifact_id = excluded.artifact_id,
    sig_artifact_id = excluded.sig_artifact_id,
    soak_started_at = excluded.soak_started_at,
    eligible_at = excluded.eligible_at,
    status = excluded.status;

-- name: UpdateSoakStatus :exec
UPDATE repo_soaks SET status = ? WHERE pkgname = ? AND arch = ? AND pkgver = ?;

-- name: DeleteSoaksForProject :exec
DELETE FROM repo_soaks WHERE project_id = ?;

-- name: GetRepoDatabase :one
SELECT * FROM repo_databases WHERE channel = ? AND arch = ?;

-- name: ListRepoDatabases :many
SELECT * FROM repo_databases ORDER BY channel, arch;

-- name: UpsertRepoDatabase :exec
INSERT INTO repo_databases (
    channel, arch, db_artifact_id, db_sig_artifact_id, files_artifact_id,
    files_sig_artifact_id, generated_at
) VALUES (?, ?, ?, ?, ?, ?, ?)
ON CONFLICT (channel, arch) DO UPDATE SET
    db_artifact_id = excluded.db_artifact_id,
    db_sig_artifact_id = excluded.db_sig_artifact_id,
    files_artifact_id = excluded.files_artifact_id,
    files_sig_artifact_id = excluded.files_sig_artifact_id,
    generated_at = excluded.generated_at;

-- name: DeleteRepoDatabase :exec
DELETE FROM repo_databases WHERE channel = ? AND arch = ?;

-- name: UpdateProjectRepo :one
UPDATE projects
SET repo_publish = ?,
    repo_pkgname_override = ?,
    repo_published_pkgname = ?,
    modified_at = ?,
    revision = revision + 1
WHERE id = ? AND revision = ?
RETURNING *;

-- name: UpdateReleasePkgrel :one
UPDATE releases
SET arch_pkgrel = ?,
    body_json = ?,
    modified_at = ?,
    revision = revision + 1
WHERE id = ?
RETURNING *;
