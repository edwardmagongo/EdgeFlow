import { Pool } from 'pg';
import { Logger, Provider } from '@nestjs/common';
import { loadConfig } from '../config';

export const PG_POOL = 'PG_POOL';

const logger = new Logger('PgPool');

export const pgPoolProvider: Provider = {
  provide: PG_POOL,
  useFactory: (): Pool => {
    const pool = new Pool({
      connectionString: loadConfig().databaseUrl,

      // Without this, an unreachable Postgres makes a query wait forever
      // rather than fail. That is not hypothetical: revoking the RDS security
      // group's ingress rule during Task 8's acceptance run left
      // GET /v1/health hanging instead of reporting "database": false, because
      // a dropped packet -- unlike a refused connection -- produces no error
      // for the endpoint's try/catch to catch.
      //
      // 3s, chosen to sit under the gateway sink's own 5s per-attempt timeout.
      // The backend then answers with its own retryable 503 before the sink
      // gives up client-side, which is a cleaner signal for the retry path.
      // Note this bound also covers waiting for a free client when the pool is
      // at capacity, so it should stay well clear of a healthy connect time.
      connectionTimeoutMillis: 3_000,
    });

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
