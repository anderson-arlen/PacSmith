-- Single-row marker that this library has been initialized. Later PKI
-- bootstrap uses this to distinguish "brand new" from "CAs went missing".
CREATE TABLE server_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    created_at TEXT NOT NULL,
    schema_kind TEXT NOT NULL
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
