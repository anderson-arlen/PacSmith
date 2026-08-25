CREATE TABLE build_artifacts (
    build_id TEXT NOT NULL REFERENCES builds (id) ON DELETE CASCADE,
    artifact_id TEXT NOT NULL REFERENCES artifacts (id),
    PRIMARY KEY (build_id, artifact_id)
);

INSERT OR IGNORE INTO build_artifacts (build_id, artifact_id)
SELECT (
           SELECT builds.id
           FROM builds
           WHERE builds.release_id = release_artifacts.release_id
             AND builds.status = 'succeeded'
           ORDER BY COALESCE(builds.finished_at, builds.started_at) DESC, builds.id DESC
           LIMIT 1
       ),
       release_artifacts.artifact_id
FROM release_artifacts
WHERE release_artifacts.role = 'built_package'
  AND EXISTS (
      SELECT 1
      FROM builds
      WHERE builds.release_id = release_artifacts.release_id
        AND builds.status = 'succeeded'
  );
