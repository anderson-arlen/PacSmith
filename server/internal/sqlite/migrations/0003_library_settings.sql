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

INSERT INTO library_settings (id) VALUES (1);
