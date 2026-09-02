-- 0001_communities — every community this instance knows about, its own included.
--
-- Comes before 0002_users because a member belongs to a community: users.community_id is
-- NOT NULL with a foreign key on this table. See ../db/communities.json for the full
-- contracted table; the columns absent here wait for the feature that reads them
-- (authenticated_at, gms_api_key, the JWT keys, hiero_topic_id, location).
--
-- The column is `remote`, where legacy has `foreign`: that is a reserved word in PostgreSQL
-- and not even parseable unquoted in SQLite.

CREATE TABLE communities (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  remote boolean NOT NULL DEFAULT true,
  url varchar(255) NOT NULL,
  public_key bytea NOT NULL,
  private_key bytea,
  community_uuid uuid NOT NULL,
  name varchar(40),
  description varchar(255),
  creation_date timestamptz(3),
  created_at timestamptz(3) NOT NULL DEFAULT now(),
  updated_at timestamptz(3)
);

CREATE UNIQUE INDEX communities_url_key ON communities (url);

-- community_uuid is NOT NULL, which is what makes this a constraint that holds: two NULLs
-- are distinct in a unique index, so a nullable column would have enforced nothing.
CREATE UNIQUE INDEX communities_uuid_key ON communities (community_uuid);

-- Legacy meant this to be unique (its migration 0065) and lost the constraint while
-- widening the column (its 0068). The key is what a federation handshake identifies a
-- community by, so two rows holding one key is an authentication ambiguity, not a duplicate.
CREATE UNIQUE INDEX communities_public_key_key ON communities (public_key);
