-- name: UpsertCredential :exec
INSERT INTO credentials (name, updated_at) VALUES (?, ?)
ON CONFLICT (name) DO UPDATE SET updated_at = excluded.updated_at;

-- name: DeleteCredentialMeta :exec
DELETE FROM credentials WHERE name = ?;

-- name: ListCredentialNames :many
SELECT name, updated_at FROM credentials ORDER BY name;

-- name: GetCredentialMeta :one
SELECT name, updated_at FROM credentials WHERE name = ?;
