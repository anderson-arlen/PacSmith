ALTER TABLE server_state ADD COLUMN secret_backend TEXT NOT NULL DEFAULT '';
ALTER TABLE server_state ADD COLUMN pki_ready INTEGER NOT NULL DEFAULT 0;

CREATE TABLE projects (
    id TEXT PRIMARY KEY NOT NULL,
    revision INTEGER NOT NULL DEFAULT 1,
    display_name TEXT NOT NULL,
    arch_package_name TEXT NOT NULL,
    vendor_name TEXT NOT NULL DEFAULT '',
    source_identity TEXT NOT NULL,
    icon_artifact_id TEXT REFERENCES artifacts (id),
    icon_sha256 TEXT NOT NULL DEFAULT '',
    history_json TEXT NOT NULL DEFAULT '[]',
    created_at TEXT NOT NULL,
    modified_at TEXT NOT NULL
);

CREATE INDEX projects_source_identity_idx ON projects (source_identity);

CREATE TABLE releases (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects (id) ON DELETE CASCADE,
    revision INTEGER NOT NULL DEFAULT 1,
    state TEXT NOT NULL,
    source_type TEXT NOT NULL,
    vendor_version TEXT NOT NULL DEFAULT '',
    original_filename TEXT NOT NULL,
    source_sha256 TEXT NOT NULL,
    source_artifact_id TEXT REFERENCES artifacts (id),
    arch_package_name TEXT NOT NULL,
    arch_pkgrel INTEGER NOT NULL DEFAULT 1,
    body_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    modified_at TEXT NOT NULL,
    UNIQUE (project_id, source_sha256)
);

CREATE INDEX releases_project_idx ON releases (project_id);

CREATE TABLE release_artifacts (
    release_id TEXT NOT NULL REFERENCES releases (id) ON DELETE CASCADE,
    artifact_id TEXT NOT NULL REFERENCES artifacts (id),
    role TEXT NOT NULL,
    PRIMARY KEY (release_id, artifact_id, role)
);

CREATE TABLE builds (
    id TEXT PRIMARY KEY NOT NULL,
    release_id TEXT NOT NULL REFERENCES releases (id) ON DELETE CASCADE,
    status TEXT NOT NULL,
    log_text TEXT NOT NULL DEFAULT '',
    started_at TEXT,
    finished_at TEXT
);

CREATE INDEX builds_release_idx ON builds (release_id);

CREATE TABLE jobs (
    id TEXT PRIMARY KEY NOT NULL,
    kind TEXT NOT NULL,
    status TEXT NOT NULL,
    project_id TEXT,
    release_id TEXT,
    payload_json TEXT NOT NULL,
    error TEXT NOT NULL DEFAULT '',
    log_offset INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    started_at TEXT,
    finished_at TEXT
);

CREATE INDEX jobs_kind_status_idx ON jobs (kind, status);

CREATE TABLE credentials (
    name TEXT PRIMARY KEY NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE registrations (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    status TEXT NOT NULL,
    csr_pem TEXT NOT NULL,
    cert_pem TEXT NOT NULL DEFAULT '',
    client_id TEXT,
    created_at TEXT NOT NULL,
    expires_at TEXT NOT NULL,
    remote_addr TEXT NOT NULL DEFAULT ''
);

CREATE INDEX registrations_status_idx ON registrations (status);

CREATE TABLE clients (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    cert_pem TEXT NOT NULL,
    cert_sha256 TEXT NOT NULL UNIQUE,
    revoked INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL
);

CREATE TABLE update_sources (
    id TEXT PRIMARY KEY NOT NULL,
    release_id TEXT NOT NULL UNIQUE REFERENCES releases (id) ON DELETE CASCADE,
    revision INTEGER NOT NULL DEFAULT 1,
    strategy TEXT NOT NULL,
    config_json TEXT NOT NULL
);

CREATE TABLE update_check_state (
    update_source_id TEXT PRIMARY KEY NOT NULL REFERENCES update_sources (id) ON DELETE CASCADE,
    last_checked_at TEXT NOT NULL DEFAULT '',
    last_message TEXT NOT NULL DEFAULT '',
    last_error TEXT NOT NULL DEFAULT '',
    detected_version TEXT NOT NULL DEFAULT '',
    detected_filename TEXT NOT NULL DEFAULT '',
    detected_sha256 TEXT NOT NULL DEFAULT '',
    detected_url TEXT NOT NULL DEFAULT '',
    etag TEXT NOT NULL DEFAULT '',
    signature_verified INTEGER NOT NULL DEFAULT 0,
    job_id TEXT NOT NULL DEFAULT ''
);
