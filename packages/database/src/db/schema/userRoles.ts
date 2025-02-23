
import { mysqlTable, index, int, varchar, datetime } from "drizzle-orm/mysql-core"
import { sql } from "drizzle-orm"

export const userRoles = mysqlTable("user_roles", {
  id: int().autoincrement().notNull(),
  userId: int("user_id").notNull(),
  role: varchar({ length: 40 }).notNull(),
  createdAt: datetime("created_at", { mode: 'string', fsp: 3 }).default(sql`current_timestamp(3)`).notNull(),
  updatedAt: datetime("updated_at", { mode: 'string', fsp: 3 }).default(sql`NULL`),
},
(table) => [
  index("user_id").on(table.userId),
]);