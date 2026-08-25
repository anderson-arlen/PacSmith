-- Keep the columns for compatibility with existing databases while making
-- persisted provider selections inert. A later schema rebuild may remove them.
UPDATE library_settings
SET ai_provider = 'none',
    ai_model = '',
    ai_reasoning_effort = 'provider-default',
    ai_execution_mode = 'standard',
    ai_auto_resolve = 0;

DELETE FROM credentials
WHERE name IN ('openai.api_key', 'xai.api_key', 'chatgpt.session');

UPDATE jobs
SET status = 'failed',
    error = 'built-in AI integration was removed; use PacSmith MCP from an external harness',
    finished_at = COALESCE(finished_at, CURRENT_TIMESTAMP)
WHERE kind IN ('ai', 'ai_github_asset') AND status IN ('queued', 'running');
