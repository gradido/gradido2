-- 0002_users — the two tables an account is made of: users and user_contacts.
--
-- users is a subset of ../db/users.json: what an account needs in order to exist and to be
-- logged into later. The rest arrive with the feature that reads them — about_me, gender,
-- salutation (profile editing), avatar_id (media storage), gms_* and humhub_* (those
-- integrations), location (open in the contract), password (legacy's shorthash, which
-- gradido2 never writes). user_contacts is the whole contracted table.
--
-- community_id is a row id and stays one. Legacy joins communities by uuid; carrying 16
-- bytes of key in every member row, every index entry and every join names a set with a
-- handful of members. The uuid is the community's federation-facing identity and lives in
-- communities.community_uuid alone. See ../db/users.json, note on the column.
--
-- The two tables point at each other: user_contacts.user_id at a member, users.email_id at
-- the row holding their login address. PostgreSQL is told about the second one after both
-- tables exist, because a foreign key cannot name a table that is not there yet.

CREATE TABLE users (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  remote boolean NOT NULL DEFAULT false,
  gradido_id uuid NOT NULL,
  community_id bigint NOT NULL REFERENCES communities (id),
  alias varchar(20),
  email_id bigint,
  first_name varchar(255),
  last_name varchar(255),
  language varchar(4) NOT NULL DEFAULT 'de',
  password_hash varchar(255),
  password_encryption_type integer NOT NULL DEFAULT 0,
  created_at timestamptz(3) NOT NULL DEFAULT now(),
  deleted_at timestamptz(3)
);

-- Both keys are per community, and that is not a detail of how they are written. A
-- table-wide unique on either column is a different rule that refuses rows the contract
-- admits: two communities may each have a member called "einhorn", and a v4 gradido_id can
-- collide across communities — unlikely, not impossible.
CREATE UNIQUE INDEX users_uuid_key ON users (gradido_id, community_id);

CREATE UNIQUE INDEX users_alias_key ON users (alias, community_id);

CREATE INDEX users_created_at_id_community_id_idx ON users (created_at, id, community_id);

CREATE TABLE user_contacts (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  user_id bigint NOT NULL REFERENCES users (id),
  type varchar(100),
  email varchar(255) NOT NULL,
  email_checked boolean NOT NULL DEFAULT false,
  email_verification_code bigint NOT NULL,
  email_opt_in_type_id integer NOT NULL DEFAULT 0,
  email_resend_count integer NOT NULL DEFAULT 0,
  gms_publish_email boolean NOT NULL DEFAULT false,
  phone varchar(255),
  country_code varchar(255),
  gms_publish_phone integer NOT NULL DEFAULT 0,
  created_at timestamptz(3) NOT NULL DEFAULT now(),
  updated_at timestamptz(3),
  deleted_at timestamptz(3)
);

-- Global, and that is the contract rather than an approximation of one: an address
-- identifies a person, and two communities cannot share it. Unlike the keys on users.
CREATE UNIQUE INDEX user_contacts_email_key ON user_contacts (email);

CREATE UNIQUE INDEX user_contacts_email_verification_code_key ON user_contacts (email_verification_code);

ALTER TABLE users ADD CONSTRAINT users_email_id_fkey FOREIGN KEY (email_id) REFERENCES user_contacts (id);
