CREATE TABLE project_repo_policies (
    project_id TEXT PRIMARY KEY NOT NULL REFERENCES projects (id) ON DELETE CASCADE,
    stable_enabled INTEGER NOT NULL DEFAULT 0,
    automatic_soak INTEGER NOT NULL DEFAULT 0,
    CHECK (automatic_soak = 0 OR stable_enabled = 1)
);

INSERT INTO project_repo_policies (project_id, stable_enabled, automatic_soak)
SELECT p.id, 1, 1
FROM projects p
WHERE p.repo_publish != 0
  AND (
      EXISTS (
          SELECT 1 FROM repo_soaks s
          WHERE s.project_id = p.id AND s.status IN ('soaking', 'eligible')
      )
      OR EXISTS (
          SELECT 1 FROM repo_channel_entries c
          WHERE c.project_id = p.id AND c.channel = 'stable'
      )
  );
