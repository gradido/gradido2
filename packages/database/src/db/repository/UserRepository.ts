import { db } from '..'
import { users } from '../schema/users'
import { userContacts } from '../schema/userContacts'
import { userRoles } from '../schema/userRoles'
import { eq } from 'drizzle-orm'

export class UserRepository {
  static async findUserByEmail(email: string): Promise<void> {
    const result = await db
      .select()
      .from(users)
      .innerJoin(userContacts, eq(users.emailId, userContacts.id))
      .innerJoin(userRoles, eq(users.id, userRoles.userId))
      .where(eq(userContacts.email, email))
    console.log(result)
  }
}