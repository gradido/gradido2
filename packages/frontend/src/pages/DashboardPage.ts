import { BasePage } from './BasePage.abstract'

class DashboardPage extends BasePage {
  constructor() {
    super()
    this.init()
  }

  init() {
    console.log('DashboardPage initialized')
    // Add your initialization code here
  }

  render() {
    return `
      <div>
        <h1>Dashboard Page</h1>
      </div>
    `
  }
}

export default DashboardPage