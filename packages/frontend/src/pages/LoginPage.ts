import { AuthLayout } from '../layouts/AuthLayout'
import { BasePage } from './BasePage.abstract'

class LoginPage extends BasePage {
  constructor() {
    super()
    this.setPublicPage(true)
    this.setLayout(AuthLayout)
    this.init()
  }

  init() {
    console.log('LoginPage initialized')
    // Add your initialization code here
  }

  render() {
    return `
      <div>
        <h1>Login Page</h1>
        <form>
          <label for="username">Username:</label>
          <input type="text" id="username" name="username">
          <label for="password">Password:</label>
          <input type="password" id="password" name="password">
          <button type="submit">Login</button>
        </form>
      </div>
    `
  }
}

export default LoginPage