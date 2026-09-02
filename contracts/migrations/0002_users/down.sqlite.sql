-- Undoes 0002_users. **This destroys every account** — see the PostgreSQL file.
--
-- SQLite cannot drop a constraint, so the reference is emptied instead of removed. With
-- foreign_keys ON a DROP TABLE performs an implicit DELETE FROM, and that delete must not
-- violate anything: users.email_id is nulled first, then the contacts go, then the tables.

UPDATE users SET email_id = NULL;

DELETE FROM user_contacts;

DROP TABLE user_contacts;

DROP TABLE users;
