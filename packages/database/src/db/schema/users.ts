
import { bigint, mysqlTable, tinyint, unique, int, varchar, char, datetime } from "drizzle-orm/mysql-core"
import { sql } from "drizzle-orm"
import { geometryType } from "../geometryType";

export const users = mysqlTable("users", {
  id: int().autoincrement().notNull(),
  foreign: tinyint().default(0).notNull(),
  gradidoId: char("gradido_id", { length: 36 }).notNull(),
  communityUuid: varchar("community_uuid", { length: 36 }).default(sql`NULL`),
  alias: varchar({ length: 20 }).default(sql`NULL`),
  emailId: int("email_id").default(sql`NULL`),
  firstName: varchar("first_name", { length: 255 }).default(sql`NULL`),
  lastName: varchar("last_name", { length: 255 }).default(sql`NULL`),
  gmsPublishName: int("gms_publish_name").default(0).notNull(),
  humhubPublishName: int("humhub_publish_name").default(0).notNull(),
  deletedAt: datetime("deleted_at", { mode: 'string', fsp: 3 }).default(sql`NULL`),
  password: bigint({ mode: "number" }),
  passwordEncryptionType: int("password_encryption_type").default(0).notNull(),
  createdAt: datetime("created_at", { mode: 'string', fsp: 3 }).default('current_timestamp(3)').notNull(),
  language: varchar({ length: 4 }).default('de').notNull(),
  referrerId: int("referrer_id").default(sql`NULL`),
  contributionLinkId: int("contribution_link_id").default(sql`NULL`),
  publisherId: int("publisher_id").default(0),
  hideAmountGdd: tinyint().default(0),
  hideAmountGdt: tinyint().default(0),
  gmsAllowed: tinyint("gms_allowed").default(1).notNull(),
  geometryType: geometryType("location"),
  gmsPublishLocation: int("gms_publish_location").default(2).notNull(),
  gmsRegistered: tinyint("gms_registered").default(0).notNull(),
  gmsRegisteredAt: datetime("gms_registered_at", { mode: 'string', fsp: 3 }).default(sql`NULL`),
  humhubAllowed: tinyint("humhub_allowed").default(0).notNull(),
},
(table) => [
  unique("uuid_key").on(table.gradidoId, table.communityUuid),
  unique("alias_key").on(table.alias, table.communityUuid),
]);
