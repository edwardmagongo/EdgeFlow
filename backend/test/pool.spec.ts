import { Pool } from 'pg';
import { pgPoolProvider } from '../src/db/pool';

// A pooled pg client that goes idle stays subscribed to the underlying
// socket. If Postgres terminates that connection out-of-band (e.g. an
// administrator shutdown), pg-pool re-emits it as an 'error' event on the
// Pool itself, asynchronously and outside any query's try/catch. Node's
// EventEmitter throws an 'error' event with no listener as an uncaught
// exception, which crashes the whole process -- this is exactly what
// happened during Task 8's acceptance run against a real Postgres outage.
// This test cannot easily reproduce the real out-of-band termination
// without a live outage, but it can prove the one thing that matters: the
// Pool built by this provider has an 'error' listener attached, so emitting
// 'error' on it does not throw/crash the test process.
describe('pgPoolProvider', () => {
  let pool: Pool;

  afterEach(async () => {
    await pool.end();
  });

  it('attaches an error listener so an idle-client error does not crash the process', () => {
    pool = (pgPoolProvider as { useFactory: () => Pool }).useFactory();

    expect(pool.listenerCount('error')).toBeGreaterThan(0);

    // Simulate the out-of-band termination pg-pool re-emits as the Pool's
    // own 'error' event. Without a listener, this line would throw.
    expect(() => {
      pool.emit('error', new Error('terminating connection due to administrator command'));
    }).not.toThrow();
  });

  it('remains usable for new queries after an idle-client error', async () => {
    pool = (pgPoolProvider as { useFactory: () => Pool }).useFactory();

    pool.emit('error', new Error('terminating connection due to administrator command'));

    const result = await pool.query('SELECT 1 AS n');
    expect(result.rows[0].n).toBe(1);
  });
});
