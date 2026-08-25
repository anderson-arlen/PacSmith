-- Current sqlc schema. Runtime applies numbered migrations; this file is the
-- end state those migrations produce and must stay in sync with them.
CREATE TABLE server_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    created_at TEXT NOT NULL,
    schema_kind TEXT NOT NULL,
    secret_backend TEXT NOT NULL DEFAULT '',
    pki_ready INTEGER NOT NULL DEFAULT 0,
    listen_enabled INTEGER NOT NULL DEFAULT 0,
    listen_port INTEGER NOT NULL DEFAULT 8443,
    listen_hosts TEXT NOT NULL DEFAULT '["0.0.0.0"]'
);

CREATE TABLE artifacts (
    id TEXT PRIMARY KEY NOT NULL,
    sha256 TEXT NOT NULL UNIQUE,
    size_bytes INTEGER NOT NULL CHECK (size_bytes >= 0),
    original_filename TEXT NOT NULL,
    kind TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE INDEX artifacts_sha256_idx ON artifacts (sha256);

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
    modified_at TEXT NOT NULL,
    repo_publish INTEGER NOT NULL DEFAULT 0,
    repo_pkgname_override TEXT NOT NULL DEFAULT '',
    repo_published_pkgname TEXT NOT NULL DEFAULT ''
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

CREATE TABLE build_artifacts (
    build_id TEXT NOT NULL REFERENCES builds (id) ON DELETE CASCADE,
    artifact_id TEXT NOT NULL REFERENCES artifacts (id),
    PRIMARY KEY (build_id, artifact_id)
);

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

CREATE TABLE library_settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    revision INTEGER NOT NULL DEFAULT 1,
    ai_provider TEXT NOT NULL DEFAULT 'none',
    ai_model TEXT NOT NULL DEFAULT '',
    ai_reasoning_effort TEXT NOT NULL DEFAULT 'provider-default',
    ai_execution_mode TEXT NOT NULL DEFAULT 'standard',
    ai_auto_resolve INTEGER NOT NULL DEFAULT 0,
    updates_enabled INTEGER NOT NULL DEFAULT 0,
    updates_daily INTEGER NOT NULL DEFAULT 1,
    updates_weekday INTEGER NOT NULL DEFAULT 1 CHECK (updates_weekday BETWEEN 1 AND 7),
    updates_hour INTEGER NOT NULL DEFAULT 2 CHECK (updates_hour BETWEEN 0 AND 23),
    updates_minute INTEGER NOT NULL DEFAULT 0 CHECK (updates_minute BETWEEN 0 AND 59),
    updates_auto_prepare INTEGER NOT NULL DEFAULT 0,
    retained_package_versions INTEGER NOT NULL DEFAULT 2,
    retained_complete_releases INTEGER NOT NULL DEFAULT 3
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

CREATE TABLE repo_settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    revision INTEGER NOT NULL DEFAULT 1,
    enabled INTEGER NOT NULL DEFAULT 0,
    listen_hosts TEXT NOT NULL DEFAULT '["127.0.0.1"]',
    listen_port INTEGER NOT NULL DEFAULT 8080,
    advertised_url TEXT NOT NULL DEFAULT '',
    soak_seconds INTEGER NOT NULL DEFAULT 2592000,
    package_name_prefix TEXT NOT NULL DEFAULT '',
    trust_mode TEXT NOT NULL DEFAULT 'direct',
    signing_fingerprint TEXT NOT NULL DEFAULT '',
    signing_initialized INTEGER NOT NULL DEFAULT 0,
    signing_pubkey_artifact_id TEXT REFERENCES artifacts (id),
    root_pubkey_artifact_id TEXT REFERENCES artifacts (id),
    root_fingerprint TEXT NOT NULL DEFAULT '',
    certified_pubkey_artifact_id TEXT REFERENCES artifacts (id),
    keyring_gpg_artifact_id TEXT REFERENCES artifacts (id),
    keyring_trusted_artifact_id TEXT REFERENCES artifacts (id),
    keyring_revoked_artifact_id TEXT REFERENCES artifacts (id),
    keyring_package_artifact_id TEXT REFERENCES artifacts (id),
    keyring_package_sig_artifact_id TEXT REFERENCES artifacts (id),
    keyring_version INTEGER NOT NULL DEFAULT 0,
    modified_at TEXT NOT NULL
);

CREATE TABLE repo_packages (
    pkgname TEXT PRIMARY KEY NOT NULL,
    project_id TEXT UNIQUE REFERENCES projects (id) ON DELETE CASCADE,
    original_pkgname TEXT NOT NULL,
    internal INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL
);

CREATE TABLE repo_channel_entries (
    channel TEXT NOT NULL CHECK (channel IN ('stable', 'unstable')),
    arch TEXT NOT NULL,
    pkgname TEXT NOT NULL,
    project_id TEXT REFERENCES projects (id) ON DELETE CASCADE,
    release_id TEXT REFERENCES releases (id) ON DELETE SET NULL,
    epoch INTEGER NOT NULL DEFAULT 0,
    pkgver TEXT NOT NULL,
    pkgrel TEXT NOT NULL,
    artifact_id TEXT NOT NULL REFERENCES artifacts (id),
    sig_artifact_id TEXT REFERENCES artifacts (id),
    filename TEXT NOT NULL,
    published_at TEXT NOT NULL,
    PRIMARY KEY (channel, arch, pkgname)
);

CREATE INDEX repo_channel_artifact_idx ON repo_channel_entries (artifact_id);

CREATE TABLE repo_soaks (
    pkgname TEXT NOT NULL,
    arch TEXT NOT NULL,
    pkgver TEXT NOT NULL,
    project_id TEXT REFERENCES projects (id) ON DELETE CASCADE,
    release_id TEXT REFERENCES releases (id) ON DELETE SET NULL,
    epoch INTEGER NOT NULL DEFAULT 0,
    pkgrel TEXT NOT NULL,
    artifact_id TEXT NOT NULL REFERENCES artifacts (id),
    sig_artifact_id TEXT REFERENCES artifacts (id),
    soak_started_at TEXT NOT NULL,
    eligible_at TEXT NOT NULL,
    status TEXT NOT NULL,
    PRIMARY KEY (pkgname, arch, pkgver)
);

CREATE INDEX repo_soaks_status_idx ON repo_soaks (status);

CREATE TABLE repo_databases (
    channel TEXT NOT NULL CHECK (channel IN ('stable', 'unstable')),
    arch TEXT NOT NULL,
    db_artifact_id TEXT NOT NULL REFERENCES artifacts (id),
    db_sig_artifact_id TEXT REFERENCES artifacts (id),
    files_artifact_id TEXT REFERENCES artifacts (id),
    files_sig_artifact_id TEXT REFERENCES artifacts (id),
    generated_at TEXT NOT NULL,
    PRIMARY KEY (channel, arch)
);
