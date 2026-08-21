-- name: GetServerState :one
SELECT id, created_at, schema_kind, secret_backend, pki_ready,
       listen_enabled, listen_port, listen_hosts
FROM server_state
WHERE id = 1;

-- name: InsertServerState :exec
INSERT INTO server_state (id, created_at, schema_kind, secret_backend, pki_ready)
VALUES (1, ?, ?, ?, ?);

-- name: UpdateServerBackend :exec
UPDATE server_state
SET secret_backend = ?, pki_ready = ?
WHERE id = 1;

-- name: UpdateServerListen :exec
UPDATE server_state
SET listen_enabled = ?, listen_port = ?, listen_hosts = ?
WHERE id = 1;
