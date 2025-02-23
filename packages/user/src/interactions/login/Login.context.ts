import { UserRepository } from 'database'
import { encode } from '../../auth/JWT'

export class LoginContext {
  public static async login(email: string, password: string): Promise<string> {
    const user = await UserRepository.findUserByEmail(email)
    // if (!user) throw new LogError('401 Unauthorized')
    // if (!user.password) throw new LogError('401 Unauthorized')
    // if (user.password !== password) throw new LogError('401 Unauthorized')
    // const token = await encode(user.id)
    // return token
    return encode('123')
  }
}