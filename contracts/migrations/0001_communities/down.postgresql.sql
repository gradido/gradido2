-- Undoes 0001_communities. The indexes go with the table.
--
-- users.community_id references this table, so 0002 has to have been undone first. That is
-- what "in reverse order" means, and why nothing here checks for it: a down run that skips a
-- step is not a case to survive, it is a bug in whatever ran it.

DROP TABLE communities;
