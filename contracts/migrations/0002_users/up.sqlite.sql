-- 0002_users — see the PostgreSQL file for what these tables are and why.
--
-- users.email_id names user_contacts before that table exists. SQLite resolves a foreign key
-- when the row is written, not when the table is created, and by then both exist — which is
-- why this file needs no counterpart to the ALTER TABLE at the end of the PostgreSQL one.

CREATE TABLE users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  remote INTEGER NOT NULL DEFAULT 0,
  gradido_id TEXT NOT NULL,
  community_id INTEGER NOT NULL REFERENCES communities (id),
  alias TEXT,
  email_id INTEGER REFERENCES user_contacts (id),
  first_name TEXT,
  last_name TEXT,
  language TEXT NOT NULL DEFAULT 'de',
  password_hash TEXT,
  password_encryption_type INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  deleted_at INTEGER
);

CREATE UNIQUE INDEX users_uuid_key ON users (gradido_id, community_id);

CREATE UNIQUE INDEX users_alias_key ON users (alias, community_id);

CREATE INDEX users_created_at_id_community_id_idx ON users (created_at, id, community_id);

CREATE TABLE user_contacts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES users (id),
  type TEXT,
  email TEXT NOT NULL,
  email_checked INTEGER NOT NULL DEFAULT 0,
  email_verification_code INTEGER NOT NULL,
  email_opt_in_type_id INTEGER NOT NULL DEFAULT 0,
  email_resend_count INTEGER NOT NULL DEFAULT 0,
  gms_publish_email INTEGER NOT NULL DEFAULT 0,
  phone TEXT,
  country_code TEXT,
  gms_publish_phone INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  updated_at INTEGER,
  deleted_at INTEGER
);

CREATE UNIQUE INDEX user_contacts_email_key ON user_contacts (email);

CREATE UNIQUE INDEX user_contacts_email_verification_code_key ON user_contacts (email_verification_code);
