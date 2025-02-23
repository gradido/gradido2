import { procedure, router } from '.'
import { z } from 'zod'
import { LoginContext } from 'user'

export const authRouter = router({
  login: procedure
    .input(z.object({
      email: z.string().email(),
      password: z.string()
    }))
    .mutation(async ({ input }) => {
      const token = await LoginContext.login(input.email, input.password)
      return token
    }),
  /*register: procedure
    .input(z.object({
      email: z.string().email(),
      username: z.string(),
      password: z.string()
    }))
    .mutation(async ({ input }) => {
      const existingUser = await db.userContacts.findOne({ email: input.email });

      if (existingUser) {
        throw new Error('User already exists');
      }

      const userContact = await db.userContacts.insert({
        email: input.email,
        username: input.username
      });

      const user = await db.users.insert({
        emailId: userContact.id,
        password: BigInt(input.password),
        gradidoId: uuid.v4()
      });

      return { userId: user.id };
    }*/
})

export type authRouter = typeof authRouter