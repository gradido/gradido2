import { customType } from 'drizzle-orm/mysql-core'

export const geometryType = customType<{ data: any }>({
  dataType() {
    return 'geometry'
  },
})

/*
const customJson = <TData>(name: string) =>
  customType<{ data: TData; driverData: string }>({
    dataType() {
      return 'json';
    },
    toDriver(value: TData): string {
      return JSON.stringify(value);
    },
  })(name);
*/