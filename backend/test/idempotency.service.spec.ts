import { IdempotencyService } from '../src/idempotency/idempotency.service';
import { MetricsService } from '../src/metrics/metrics.service';

describe('IdempotencyService', () => {
  let metrics: MetricsService;
  let service: IdempotencyService;

  beforeEach(async () => {
    metrics = new MetricsService();
    service = new IdempotencyService(metrics, {
      redisUrl: process.env.REDIS_URL as string,
      idempotencyTtlSeconds: 900,
    });
    await service.connect();
  });

  afterEach(async () => {
    await service.onModuleDestroy();
  });

  function uniqueKey(): string {
    return `test-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  }

  it('claims an unseen key', async () => {
    expect(await service.claim(uniqueKey())).toBe('claimed');
  });

  it('reports the second claim of the same key as a duplicate', async () => {
    const key = uniqueKey();
    expect(await service.claim(key)).toBe('claimed');
    expect(await service.claim(key)).toBe('duplicate');
  });

  it('treats distinct keys independently', async () => {
    expect(await service.claim(uniqueKey())).toBe('claimed');
    expect(await service.claim(uniqueKey())).toBe('claimed');
  });

  it('sets a TTL so keys do not accumulate forever', async () => {
    const key = uniqueKey();
    await service.claim(key);
    const ttl = await service.ttlForTesting(key);
    // A missing TTL comes back as -1, which would mean keys live forever and
    // Redis grows without bound at ~1,966 batches/sec.
    expect(ttl).toBeGreaterThan(0);
    expect(ttl).toBeLessThanOrEqual(900);
  });

  it('reports reachability', async () => {
    expect(await service.isReachable()).toBe(true);
  });

  it('fails open when Redis is unreachable', async () => {
    // A port nothing is listening on stands in for an outage.
    const offline = new IdempotencyService(metrics, {
      redisUrl: 'redis://127.0.0.1:6390',
      idempotencyTtlSeconds: 900,
    });
    await offline.connect(); // must not throw

    expect(await offline.claim(uniqueKey())).toBe('unavailable');
    expect(metrics.snapshot().redis_unavailable).toBeGreaterThan(0);
    expect(await offline.isReachable()).toBe(false);

    await offline.onModuleDestroy();
  });
});
