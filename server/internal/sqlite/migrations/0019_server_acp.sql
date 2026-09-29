CREATE TABLE acp_settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    revision INTEGER NOT NULL DEFAULT 1,
    profile_json TEXT NOT NULL DEFAULT '{}'
);
INSERT INTO acp_settings(id) VALUES (1);
CREATE TABLE acp_conversations (
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL DEFAULT '',
    release_id TEXT NOT NULL DEFAULT '',
    automatic INTEGER NOT NULL DEFAULT 0,
    title TEXT NOT NULL DEFAULT '',
    profile_json TEXT NOT NULL,
    session_id TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'idle',
    config_json TEXT NOT NULL DEFAULT '[]',
    error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE INDEX acp_conversations_project ON acp_conversations(project_id, created_at DESC);
CREATE UNIQUE INDEX acp_automatic_active ON acp_conversations(release_id) WHERE automatic = 1 AND status IN ('starting', 'running', 'waiting');
CREATE TABLE acp_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id TEXT NOT NULL REFERENCES acp_conversations(id) ON DELETE CASCADE,
    kind TEXT NOT NULL,
    body_json TEXT NOT NULL
);
CREATE INDEX acp_events_conversation ON acp_events(conversation_id, id);
CREATE TABLE acp_permissions (
    id TEXT PRIMARY KEY,
    conversation_id TEXT NOT NULL REFERENCES acp_conversations(id) ON DELETE CASCADE,
    request_json TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    option_id TEXT NOT NULL DEFAULT '',
    answered_by TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    resolved_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX acp_permissions_pending ON acp_permissions(status);
CREATE TABLE acp_grants (
    identity TEXT NOT NULL,
    tool TEXT NOT NULL,
    session_id TEXT NOT NULL DEFAULT '',
    PRIMARY KEY(identity, tool, session_id)
);
