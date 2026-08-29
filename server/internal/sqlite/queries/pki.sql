-- name: InsertRegistration :one
INSERT INTO registrations (
    id, name, status, csr_pem, cert_pem, client_id, created_at, expires_at, remote_addr
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
RETURNING id, name, status, csr_pem, cert_pem, client_id, created_at, expires_at, remote_addr;

-- name: GetRegistration :one
SELECT id, name, status, csr_pem, cert_pem, client_id, created_at, expires_at, remote_addr
FROM registrations
WHERE id = ?;

-- name: ListPendingRegistrations :many
SELECT id, name, status, csr_pem, cert_pem, client_id, created_at, expires_at, remote_addr
FROM registrations
WHERE status = 'pending'
ORDER BY created_at;

-- name: CountPendingRegistrations :one
SELECT COUNT(*) FROM registrations WHERE status = 'pending';

-- name: UpdateRegistration :one
UPDATE registrations
SET status = ?,
    cert_pem = ?,
    client_id = ?
WHERE id = ?
RETURNING id, name, status, csr_pem, cert_pem, client_id, created_at, expires_at, remote_addr;

-- name: InsertClient :one
INSERT INTO clients (id, name, cert_pem, cert_sha256, revoked, created_at)
VALUES (?, ?, ?, ?, 0, ?)
RETURNING id, name, cert_pem, cert_sha256, revoked, created_at;

-- name: GetClient :one
SELECT id, name, cert_pem, cert_sha256, revoked, created_at
FROM clients
WHERE id = ?;

-- name: GetClientByCertSHA256 :one
SELECT id, name, cert_pem, cert_sha256, revoked, created_at
FROM clients
WHERE cert_sha256 = ?;

-- name: ListClients :many
SELECT id, name, cert_pem, cert_sha256, revoked, created_at
FROM clients
ORDER BY created_at;

-- name: RevokeClient :one
UPDATE clients
SET revoked = 1
WHERE id = ?
RETURNING id, name, cert_pem, cert_sha256, revoked, created_at;

-- name: DeleteAllRegistrations :exec
DELETE FROM registrations;

-- name: DeleteAllClients :exec
DELETE FROM clients;
