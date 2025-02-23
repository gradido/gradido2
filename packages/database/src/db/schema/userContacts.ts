
import { bigint, mysqlTable, tinyint, unique, int, varchar, datetime } from "drizzle-orm/mysql-core"
import { sql } from "drizzle-orm"

export const userContacts = mysqlTable("user_contacts", {
  id: int().autoincrement().notNull(),
  type: varchar({ length: 100 }).notNull(),
  userId: int("user_id").notNull(),
  email: varchar({ length: 255 }).notNull(),
  emailVerificationCode: bigint("email_verification_code", { mode: "number" }).default(sql`NULL`),
  emailOptInTypeId: int("email_opt_in_type_id").default(sql`NULL`),
  emailResendCount: int("email_resend_count").default(0),
  emailChecked: tinyint("email_checked").default(0).notNull(),
  gmsPublishEmail: tinyint("gms_publish_email").default(0).notNull(),
  countryCode: varchar("country_code", { length: 255 }).default(sql`NULL`),
  phone: varchar({ length: 255 }).default(sql`NULL`),
  gmsPublishPhone: int("gms_publish_phone").default(0).notNull(),
  createdAt: datetime("created_at", { mode: 'string', fsp: 3 }).default(sql`current_timestamp(3)`).notNull(),
  updatedAt: datetime("updated_at", { mode: 'string', fsp: 3 }).default(sql`NULL`),
  deletedAt: datetime("deleted_at", { mode: 'string', fsp: 3 }).default(sql`NULL`),
},
(table) => [
  unique("email").on(table.email),
  unique("email_verification_code").on(table.emailVerificationCode),
]);
