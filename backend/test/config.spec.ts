import { loadConfig } from '../src/config';

const base = {
  DATABASE_URL: 'postgres://u:p@localhost:5433/db',
  REDIS_URL: 'redis://localhost:6380',
};

describe('loadConfig numeric validation', () => {
  it('rejects a non-numeric TTL instead of silently producing NaN', () => {
    // Number('15m') is NaN. Unguarded, that reaches Redis as `SET ... EX NaN`,
    // which errors on every call, so claim() returns 'unavailable' forever and
    // deduplication is silently off.
    expect(() =>
      loadConfig({ ...base, IDEMPOTENCY_TTL_SECONDS: '15m' } as NodeJS.ProcessEnv),
    ).toThrow(/IDEMPOTENCY_TTL_SECONDS/);
  });

  it('rejects a non-numeric PORT instead of binding a random ephemeral port', () => {
    expect(() => loadConfig({ ...base, PORT: ':3000' } as NodeJS.ProcessEnv)).toThrow(/PORT/);
  });

  it('rejects a zero or negative TTL', () => {
    expect(() =>
      loadConfig({ ...base, IDEMPOTENCY_TTL_SECONDS: '0' } as NodeJS.ProcessEnv),
    ).toThrow(/IDEMPOTENCY_TTL_SECONDS/);
  });

  it('still applies the documented defaults when the vars are absent', () => {
    const config = loadConfig(base as NodeJS.ProcessEnv);
    expect(config.port).toBe(3000);
    expect(config.idempotencyTtlSeconds).toBe(900);
  });
});
