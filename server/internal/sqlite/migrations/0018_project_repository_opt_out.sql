-- A saved repository policy records a project-level choice; untouched projects
-- previously inherited publication off and now inherit publication on.
UPDATE projects
SET repo_publish = 1, revision = revision + 1
WHERE repo_publish = 0
  AND NOT EXISTS (
      SELECT 1 FROM project_repo_policies policy WHERE policy.project_id = projects.id
  );
