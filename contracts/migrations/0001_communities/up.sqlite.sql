-- 0001_communities — see the PostgreSQL file for what this table is and why.

CREATE TABLE communities (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  remote INTEGER NOT NULL DEFAULT 1,
  url TEXT NOT NULL,
  public_key BLOB NOT NULL,
  private_key BLOB,
  community_uuid TEXT NOT NULL,
  name TEXT,
  description TEXT,
  creation_date INTEGER,
  created_at INTEGER NOT NULL,
  updated_at INTEGER
);

CREATE UNIQUE INDEX communities_url_key ON communities (url);

CREATE UNIQUE INDEX communities_uuid_key ON communities (community_uuid);

CREATE UNIQUE INDEX communities_public_key_key ON communities (public_key);
