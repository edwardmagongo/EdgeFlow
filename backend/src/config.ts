export interface Config {
  databaseUrl: string;
  redisUrl: string;
  port: number;
  idempotencyTtlSeconds: number;
}

// 15 minutes. The worst-case retry window for one batch is about 21 seconds
// (4 attempts at the sink's 5s timeout, plus 100+200+400ms of backoff), so this
// is roughly 43x margin. The upper bound is memory, not correctness: at the
// gateway's measured ~1,966 batches/sec a 24h TTL would hold ~170M keys, on the
// order of 10-17GB. 900s holds ~1.8M keys, on the order of 150MB.
const DEFAULT_IDEMPOTENCY_TTL_SECONDS = 900;

// Number('15m') is NaN, and NaN propagates silently: SET ... EX NaN fails on
// every call, so claim() returns 'unavailable' forever and deduplication is
// off with nothing but a rising redis_unavailable to show for it. Fail at
// boot instead, which is the whole reason loadConfig() exists.
function positiveNumber(raw: string | undefined, fallback: number, name: string): number {
  if (raw === undefined) {
    return fallback;
  }
  const value = Number(raw);
  if (!Number.isFinite(value) || value <= 0) {
    throw new Error(`${name} must be a positive number, got: ${raw}`);
  }
  return value;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const databaseUrl = env.DATABASE_URL;
  if (!databaseUrl) {
    throw new Error('DATABASE_URL is required');
  }
  const redisUrl = env.REDIS_URL;
  if (!redisUrl) {
    throw new Error('REDIS_URL is required');
  }
  return {
    databaseUrl,
    redisUrl,
    port: positiveNumber(env.PORT, 3000, 'PORT'),
    idempotencyTtlSeconds: positiveNumber(
      env.IDEMPOTENCY_TTL_SECONDS,
      DEFAULT_IDEMPOTENCY_TTL_SECONDS,
      'IDEMPOTENCY_TTL_SECONDS',
    ),
  };
}
