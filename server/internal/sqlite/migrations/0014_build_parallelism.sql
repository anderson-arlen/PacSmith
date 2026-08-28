ALTER TABLE library_settings
ADD COLUMN build_parallelism INTEGER NOT NULL DEFAULT 1
CHECK (build_parallelism BETWEEN 1 AND 1024);
