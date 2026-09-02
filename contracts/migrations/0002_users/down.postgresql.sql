-- Undoes 0002_users.
--
-- **This destroys every account.** A down step is not always the inverse of an up step:
-- CREATE TABLE has one, and the rows that were written into it do not. Running this is a
-- decision about data, not about schema.
--
-- users.email_id has to stop referencing user_contacts before either table can go — the two
-- point at each other, which is also why the up step adds that constraint last.

ALTER TABLE users DROP CONSTRAINT users_email_id_fkey;

DROP TABLE user_contacts;

DROP TABLE users;
