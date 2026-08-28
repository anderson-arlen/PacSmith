ALTER TABLE repo_settings
ADD COLUMN stable_enabled INTEGER NOT NULL DEFAULT 0
CHECK (stable_enabled IN (0, 1));

UPDATE project_repo_policies SET stable_enabled = 1;
