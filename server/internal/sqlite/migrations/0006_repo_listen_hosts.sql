ALTER TABLE repo_settings ADD COLUMN listen_hosts TEXT NOT NULL DEFAULT '["127.0.0.1"]';
UPDATE repo_settings SET listen_hosts = json_array(listen_host) WHERE listen_host != '';
