ALTER TABLE library_settings
ADD COLUMN retention_versions INTEGER NOT NULL DEFAULT 2
CHECK (retention_versions >= -1);
