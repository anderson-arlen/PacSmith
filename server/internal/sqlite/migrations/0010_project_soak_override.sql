ALTER TABLE project_repo_policies
ADD COLUMN soak_seconds_override INTEGER NOT NULL DEFAULT -1
CHECK (soak_seconds_override >= -1);
