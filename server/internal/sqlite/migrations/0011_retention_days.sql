ALTER TABLE library_settings
ADD COLUMN retention_days INTEGER NOT NULL DEFAULT 30
CHECK (retention_days >= -1);
