import { Pool } from 'pg';
import { Logger, Provider } from '@nestjs/common';
import { loadConfig } from '../config';

export const PG_POOL = 'PG_POOL';

const logger = new Logger('PgPool');

export const pgPoolProvider: Provider = {
  provide: PG_POOL,
  useFactory: (): Pool => {
    const pool = new Pool({ connectionString: loadConfig().databaseUrl });

    // pg emits 'error' on the Pool itself when an already-idle pooled client
    // is terminated out-of-band (e.g. Postgres shutting down or killing the
    // connection) -- this happens outside any in-flight query's try/catch. An
    // unhandled 'error' event on a Node EventEmitter is thrown as an uncaught
    // exception, which would crash the whole process. Log and swallow it here
    // instead, mirroring the same defensive pattern already used for the
    // Redis client in idempotency.service.ts. A query issued while Postgres is
    // down still rejects normally and is caught by EventsRepository /
    // IngestController's existing 503 mapping; this handler only prevents the
    // separate idle-connection crash.
    pool.on('error', (err) => {
      logger.error(`idle client error: ${err.message}`, err.stack);
    });

    return pool;
  },
};
