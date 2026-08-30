import { execSync } from 'node:child_process';
import { IdempotencyService } from '../src/idempotency/idempotency.service';
import { MetricsService } from '../src/metrics/metrics.service';

const REDIS_URL = process.env.REDIS_URL ?? 'redis://localhost:6380';

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

  it('reports the second claim of a held key as in_flight', async () => {
    const key = uniqueKey();
    expect(await service.claim(key)).toBe('claimed');
    // Was 'duplicate' before Phase 7. A held-but-uncommitted key means another
    // attempt is mid-insert, which is NOT the same as the batch being stored.
    expect(await service.claim(key)).toBe('in_flight');
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

  it('makes a claimed key claimable again after release', async () => {
    // This is the regression test for the ingest data-loss defect: release()
    // is what undoes a claim() when the insert it was guarding fails, so a
    // retry with the same key is a genuine retry rather than a suppressed
    // duplicate.
    const key = uniqueKey();
    expect(await service.claim(key)).toBe('claimed');
    expect(await service.claim(key)).toBe('in_flight');

    await service.release(key);

    expect(await service.claim(key)).toBe('claimed');
  });

  it('does not throw when releasing against an unreachable Redis', async () => {
    // release() is called from IngestService's catch block, which must not
    // let a release failure mask the original database error. It has to be
    // safe to call even when there is no client to talk to.
    const offline = new IdempotencyService(metrics, {
      redisUrl: 'redis://127.0.0.1:6390',
      idempotencyTtlSeconds: 900,
    });
    await offline.connect(); // must not throw

    await expect(offline.release(uniqueKey())).resolves.toBeUndefined();

    await offline.onModuleDestroy();
  });

  it('fails open when Redis is unreachable', async () => {
    // A port nothing is listening on stands in for an outage. This exercises
    // claim()'s real body (not a mock of the method), so it is the test that
    // proves redis_unavailable is incremented by the one call site that
    // should own it: IdempotencyService.claim() itself. `metrics` is a fresh
    // MetricsService for this test (see beforeEach), so a single claim()
    // call must move the counter by exactly 1 -- not 0, and not more than 1.
    const offline = new IdempotencyService(metrics, {
      redisUrl: 'redis://127.0.0.1:6390',
      idempotencyTtlSeconds: 900,
    });
    await offline.connect(); // must not throw

    expect(await offline.claim(uniqueKey())).toBe('unavailable');
    expect(metrics.snapshot().redis_unavailable).toBe(1);
    expect(await offline.isReachable()).toBe(false);

    await offline.onModuleDestroy();
  });
});

describe('three-state claim', () => {
  let metrics: MetricsService;
  let service: IdempotencyService;

  beforeEach(async () => {
    metrics = new MetricsService();
    service = new IdempotencyService(metrics, {
      redisUrl: REDIS_URL,
      idempotencyTtlSeconds: 900,
    });
    await service.connect();
  });

  afterEach(async () => {
    await service.onModuleDestroy();
  });

  function uniqueKey(): string {
    return `state-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  }

  it('reports in_flight while a claim is held, not duplicate', async () => {
    // The defect this phase exists to close: with a boolean key the second
    // claim said 'duplicate', the endpoint answered 200, and the sink dropped
    // a batch that was never stored.
    const key = uniqueKey();
    expect(await service.claim(key)).toBe('claimed');
    expect(await service.claim(key)).toBe('in_flight');
  });

  it('reports committed only after markCommitted', async () => {
    const key = uniqueKey();
    expect(await service.claim(key)).toBe('claimed');
    await service.markCommitted(key);
    expect(await service.claim(key)).toBe('committed');
  });

  it('gives the in-flight lease a short TTL and the committed state the full one', async () => {
    const key = uniqueKey();
    await service.claim(key);
    const leaseTtl = await service.ttlForTesting(key);
    expect(leaseTtl).toBeGreaterThan(0);
    expect(leaseTtl).toBeLessThanOrEqual(15);

    await service.markCommitted(key);
    const committedTtl = await service.ttlForTesting(key);
    expect(committedTtl).toBeGreaterThan(15);
  });

  it('lets a fresh claim through once the in-flight lease expires', async () => {
    // Crash recovery: a holder that dies mid-insert must not block the key
    // forever. Uses a deliberately tiny lease rather than sleeping 15s.
    const shortLease = new IdempotencyService(metrics, {
      redisUrl: REDIS_URL,
      idempotencyTtlSeconds: 900,
      inFlightLeaseSeconds: 1,
    });
    await shortLease.connect();
    const key = uniqueKey();
    expect(await shortLease.claim(key)).toBe('claimed');
    expect(await shortLease.claim(key)).toBe('in_flight');
    await new Promise((resolve) => setTimeout(resolve, 1500));
    expect(await shortLease.claim(key)).toBe('claimed');
    await shortLease.onModuleDestroy();
  });

  it('release makes a held key claimable again', async () => {
    const key = uniqueKey();
    expect(await service.claim(key)).toBe('claimed');
    await service.release(key);
    expect(await service.claim(key)).toBe('claimed');
  });

  it('markCommitted on an unreachable Redis does not throw', async () => {
    const dead = new IdempotencyService(metrics, {
      redisUrl: 'redis://localhost:6390',
      idempotencyTtlSeconds: 900,
      inFlightLeaseSeconds: 15,
    });
    await dead.connect();
    await expect(dead.markCommitted('anything')).resolves.toBeUndefined();
    await dead.onModuleDestroy();
  });
});

describe('reconnect', () => {
  // Restarting the container takes a few seconds; the default 5s timeout is
  // not enough and a too-short timeout here would look like a code failure.
  jest.setTimeout(60_000);

  it('resumes deduplicating after Redis restarts, without a process restart', async () => {
    const metrics = new MetricsService();
    const service = new IdempotencyService(metrics, {
      redisUrl: REDIS_URL,
      idempotencyTtlSeconds: 900,
      inFlightLeaseSeconds: 15,
    });
    await service.connect();

    const before = `reconnect-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    expect(await service.claim(before)).toBe('claimed');

    execSync('docker compose restart redis', {
      cwd: `${__dirname}/../..`,
      stdio: 'ignore',
    });

    // Poll rather than sleeping a fixed amount: how long the container takes
    // to come back is not something this test should have to guess.
    let recovered = false;
    for (let attempt = 0; attempt < 60; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 500));
      if (await service.isReachable()) {
        recovered = true;
        break;
      }
    }
    expect(recovered).toBe(true);

    // The real assertion: claim() works again on the SAME service instance.
    // With reconnectStrategy:false this stays 'unavailable' forever.
    const after = `reconnect-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    expect(await service.claim(after)).toBe('claimed');

    await service.onModuleDestroy();
  });
});
