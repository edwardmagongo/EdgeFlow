import { Pool } from 'pg';
import { Provider } from '@nestjs/common';
import { loadConfig } from '../config';

export const PG_POOL = 'PG_POOL';

export const pgPoolProvider: Provider = {
  provide: PG_POOL,
  useFactory: (): Pool => new Pool({ connectionString: loadConfig().databaseUrl }),
};
