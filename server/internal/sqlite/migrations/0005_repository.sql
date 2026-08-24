ALTER TABLE projects ADD COLUMN repo_publish INTEGER NOT NULL DEFAULT 0;
ALTER TABLE projects ADD COLUMN repo_pkgname_override TEXT NOT NULL DEFAULT '';
ALTER TABLE projects ADD COLUMN repo_published_pkgname TEXT NOT NULL DEFAULT '';

CREATE TABLE repo_settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    revision INTEGER NOT NULL DEFAULT 1,
    enabled INTEGER NOT NULL DEFAULT 0,
    listen_host TEXT NOT NULL DEFAULT '127.0.0.1',
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

INSERT INTO repo_settings (id, modified_at) VALUES (1, '1970-01-01T00:00:00.000Z');

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
