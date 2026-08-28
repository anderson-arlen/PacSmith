ALTER TABLE projects
ADD COLUMN auto_build_policy TEXT NOT NULL DEFAULT 'review_free'
CHECK (auto_build_policy IN ('never', 'review_free', 'ai'));

ALTER TABLE projects
ADD COLUMN compile_cache_policy TEXT NOT NULL DEFAULT 'reuse'
CHECK (compile_cache_policy IN ('reuse', 'clear_after_success', 'disabled'));

UPDATE projects
SET auto_build_policy = 'never'
WHERE EXISTS (
    SELECT 1 FROM releases
    WHERE releases.project_id = projects.id
      AND json_extract(releases.body_json, '$.pkgbuildManuallyModified') = 1
);
