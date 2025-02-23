import m from 'mithril'


export class AuthService {
  static isAuthenticated(): boolean {
    return !!localStorage.getItem('token')
  }

  static async login(username: string, password: string) {
    try {
      const response = await client.mutation('login', { username, password })
      localStorage.setItem('token', response)
      console.log(response)
      m.route.set('/dashboard')
    } catch (error) {
      console.error('Login failed', error)
    }
  }

  static async logout() {
    localStorage.removeItem('token')
    m.route.set('/login')
  }

  static async register(username: string, password: string) {
    try {
      await client.mutation('register', { username, password })
      m.route.set('/login')
    } catch (error) {
      console.error('Registration failed', error)
    }
  }

  static async getUser() {
    try {
      const user = await client.query('getUser')
      return user
    } catch (error) {
      console.error('Fetching user failed', error)
      return null
    }
  }
}