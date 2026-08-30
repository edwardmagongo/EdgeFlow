import { execSync } from 'node:child_process';
import { IdempotencyService } from '../src/idempotency/idempotency.service';
import { MetricsService } from '../src/metrics/metrics.service';

const REDIS_URL = process.env.REDIS_URL ?? 'redis://localhost:6380';

// claim() returns a discriminated union now, because only 'claimed' carries the
// lease token that release()/markCommitted() compare against. Narrowing here
// keeps every call site that needs the token from repeating the same guard.
async function claimToken(service: IdempotencyService, key: string): Promise<string> {
  const result = await service.claim(key);
  if (result.state !== 'claimed') {
    throw new Error(`expected 'claimed' for ${key}, got '${result.state}'`);
  }
  return result.token;
}

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
    expect((await service.claim(uniqueKey())).state).toBe('claimed');
  });

  it('reports the second claim of a held key as in_flight', async () => {
    const key = uniqueKey();
    expect((await service.claim(key)).state).toBe('claimed');
    // Was 'duplicate' before Phase 7. A held-but-uncommitted key means another
    // attempt is mid-insert, which is NOT the same as the batch being stored.
    expect((await service.claim(key)).state).toBe('in_flight');
  });

  it('treats distinct keys independently', async () => {
    expect((await service.claim(uniqueKey())).state).toBe('claimed');
    expect((await service.claim(uniqueKey())).state).toBe('claimed');
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
    const token = await claimToken(service, key);
    expect((await service.claim(key)).state).toBe('in_flight');

    await service.release(key, token);

    expect((await service.claim(key)).state).toBe('claimed');
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

    await expect(offline.release(uniqueKey(), 'any-token')).resolves.toBeUndefined();

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

    expect((await offline.claim(uniqueKey())).state).toBe('unavailable');
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
    expect((await service.claim(key)).state).toBe('claimed');
    expect((await service.claim(key)).state).toBe('in_flight');
  });

  it('reports committed only after markCommitted', async () => {
    const key = uniqueKey();
    const token = await claimToken(service, key);
    await service.markCommitted(key, token);
    expect((await service.claim(key)).state).toBe('committed');
  });

  it('gives the in-flight lease a short TTL and the committed state the full one', async () => {
    const key = uniqueKey();
    const token = await claimToken(service, key);
    const leaseTtl = await service.ttlForTesting(key);
    expect(leaseTtl).toBeGreaterThan(0);
    expect(leaseTtl).toBeLessThanOrEqual(15);

    await service.markCommitted(key, token);
    const committedTtl = await service.ttlForTesting(key);
    expect(committedTtl).toBeGreaterThan(15);
  });

  it('leaves a committed key untouched when a duplicate claim arrives', async () => {
    // NX is the single most load-bearing option in the phase, and until this
    // test nothing pinned it: deleting `NX: true` left the whole suite green,
    // because every assertion checked what the duplicate claim RETURNED (still
    // 'committed', since GET reports the pre-overwrite value) and none checked
    // what it LEFT BEHIND. Without NX the duplicate check overwrites the
    // committed record with a fresh 15s in-flight lease -- the deduplication
    // record is destroyed, and 15 seconds later a retry claims the key and
    // stores the batch a second time.
    const key = uniqueKey();
    const token = await claimToken(service, key);
    await service.markCommitted(key, token);

    expect((await service.claim(key)).state).toBe('committed');

    // The residual state, which is what the returned value cannot prove.
    const ttlAfter = await service.ttlForTesting(key);
    expect(ttlAfter).toBeGreaterThan(15); // not reset to the 15s lease
    expect(ttlAfter).toBeLessThanOrEqual(900);
    // And the stored value is still 'committed', not an in-flight lease.
    expect((await service.claim(key)).state).toBe('committed');
  });

  it('refuses to release a lease this attempt no longer owns', async () => {
    // Past lease expiry, an attempt that is still alive can otherwise DEL a
    // lease a DIFFERENT attempt legitimately holds, which lets a concurrent
    // retry insert the same batch a second time -- the silent-loss window this
    // phase closes, reopened just past the 15s boundary. release() is a
    // compare-and-delete against the token claim() minted.
    const key = uniqueKey();
    const staleToken = await claimToken(service, key);
    await service.release(key, staleToken); // the stale holder's own lease goes

    const currentToken = await claimToken(service, key); // a new attempt takes it
    await service.release(key, staleToken); // stale holder tries again, too late

    // The current holder still owns the key.
    expect((await service.claim(key)).state).toBe('in_flight');
    await service.release(key, currentToken);
    expect((await service.claim(key)).state).toBe('claimed');
  });

  it('refuses to promote a lease this attempt no longer owns', async () => {
    // The mirror image: an attempt whose lease expired must not mark the key
    // committed on behalf of whoever holds it now. Doing so would answer a
    // third attempt with 200 duplicate:true for a batch the current holder may
    // still fail to store.
    const key = uniqueKey();
    const staleToken = await claimToken(service, key);
    await service.release(key, staleToken);
    const currentToken = await claimToken(service, key);

    await service.markCommitted(key, staleToken);

    // Still in flight, not committed: the stale attempt's promotion was refused.
    expect((await service.claim(key)).state).toBe('in_flight');
    expect(await service.ttlForTesting(key)).toBeLessThanOrEqual(15);

    // The real holder can still promote it.
    await service.markCommitted(key, currentToken);
    expect((await service.claim(key)).state).toBe('committed');
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
    expect((await shortLease.claim(key)).state).toBe('claimed');
    expect((await shortLease.claim(key)).state).toBe('in_flight');
    await new Promise((resolve) => setTimeout(resolve, 1500));
    expect((await shortLease.claim(key)).state).toBe('claimed');
    await shortLease.onModuleDestroy();
  });

  it('release makes a held key claimable again', async () => {
    const key = uniqueKey();
    const token = await claimToken(service, key);
    await service.release(key, token);
    expect((await service.claim(key)).state).toBe('claimed');
  });

  it('markCommitted on an unreachable Redis does not throw', async () => {
    const dead = new IdempotencyService(metrics, {
      redisUrl: 'redis://localhost:6390',
      idempotencyTtlSeconds: 900,
      inFlightLeaseSeconds: 15,
    });
    await dead.connect();
    await expect(dead.markCommitted('anything', 'any-token')).resolves.toBeUndefined();
    await dead.onModuleDestroy();
  });
});

describe('reconnect', () => {
  // Stopping and starting the container, plus a deliberate multi-second outage,
  // takes far longer than the default timeout; a too-short one here would look
  // like a code failure.
  jest.setTimeout(120_000);

  // Long enough for the capped backoff to run several times over. The previous
  // version of this test used `docker compose restart`, which returned and was
  // reachable again inside 500ms -- at that duration the strategy is consulted
  // once or twice and never approaches its 2s cap, so "bounded, capped, retries
  // indefinitely" was asserted but untested.
  const OUTAGE_MS = 6_000;
  const COMPOSE_CWD = `${__dirname}/../..`;

  function compose(command: string): void {
    execSync(`docker compose ${command}`, { cwd: COMPOSE_CWD, stdio: 'ignore' });
  }

  function key(prefix: string): string {
    return `${prefix}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  }

  // Races a call against a deadline instead of just timing it. A guard on
  // `isOpen` does not make claim() return late -- it makes it never return
  // until Redis is back, so a plain `await` would hang the whole test run
  // rather than failing it. Resolving to TIMED_OUT turns that into a clean,
  // immediate assertion failure.
  const TIMED_OUT = Symbol('timed out');
  async function withDeadline<T>(work: Promise<T>, ms: number): Promise<T | typeof TIMED_OUT> {
    let timer: NodeJS.Timeout;
    const deadline = new Promise<typeof TIMED_OUT>((resolve) => {
      timer = setTimeout(() => resolve(TIMED_OUT), ms);
    });
    return Promise.race([work, deadline]).finally(() => clearTimeout(timer));
  }

  async function pollUntilReachable(
    service: IdempotencyService,
    budgetMs: number,
  ): Promise<number | null> {
    const started = Date.now();
    while (Date.now() - started < budgetMs) {
      await new Promise((resolve) => setTimeout(resolve, 250));
      if (await service.isReachable()) return Date.now() - started;
    }
    return null;
  }

  afterEach(() => {
    // Never leave the shared container down for the rest of the suite, even if
    // an assertion above threw mid-outage.
    compose('start redis');
  });

  it('degrades to unavailable during a real outage and recovers after it, without a process restart', async () => {
    const metrics = new MetricsService();
    const service = new IdempotencyService(metrics, {
      redisUrl: REDIS_URL,
      idempotencyTtlSeconds: 900,
      inFlightLeaseSeconds: 15,
    });
    await service.connect();

    expect((await service.claim(key('reconnect-before'))).state).toBe('claimed');
    const attemptsBefore = service.reconnectAttemptsForTesting();

    compose('stop redis');

    // The assertion the spec's Testing section asks for and the previous test
    // omitted. node-redis keeps isOpen true through the whole reconnect cycle,
    // so a guard on isOpen let the command into the offline queue and the call
    // HUNG for the length of the outage instead of failing open -- worse than
    // the fail-open it replaced, because a blocked ingest request burns the
    // sink's entire retry budget. The elapsed-time bound is what actually pins
    // that: a blocking claim() satisfies `state === 'unavailable'` too, just
    // seconds later.
    const duringOutage = await withDeadline(service.claim(key('reconnect-during')), 1_000);
    expect(duringOutage).not.toBe(TIMED_OUT);
    expect(duringOutage).toEqual({ state: 'unavailable' });

    // isReachable() must fail fast for the same reason: GET /v1/health calls it,
    // so a blocking ping takes the health endpoint down with the outage.
    expect(await withDeadline(service.isReachable(), 1_000)).toBe(false);

    // Hold the outage open long enough that the strategy is consulted many
    // times and reaches its cap, rather than getting lucky on a first retry.
    await new Promise((resolve) => setTimeout(resolve, OUTAGE_MS));
    expect(service.reconnectAttemptsForTesting()).toBeGreaterThan(attemptsBefore + 1);

    compose('start redis');

    // Bounded: with a 2s cap the client must be back within a few seconds of
    // Redis returning, not eventually.
    const recoveredAfterMs = await pollUntilReachable(service, 15_000);
    expect(recoveredAfterMs).not.toBeNull();

    // The real assertion: claim() works again on the SAME service instance.
    // With reconnectStrategy:false this stays 'unavailable' forever.
    expect((await service.claim(key('reconnect-after'))).state).toBe('claimed');

    await service.onModuleDestroy();
  });

  it('recovers when Redis is unreachable at the moment it first connects', async () => {
    // The regression this test exists for: a two-regime strategy that gave up
    // after a couple of tries while no connection had ever succeeded moved the
    // permanent fail-open from "after a Redis restart" to "Redis was not up yet
    // at boot" -- the more likely case, since compose and k8s race the backend
    // against Redis. connect() must return promptly (boot is not held hostage)
    // AND the client must still be retrying when Redis appears.
    compose('stop redis');

    const metrics = new MetricsService();
    const service = new IdempotencyService(metrics, {
      redisUrl: REDIS_URL,
      idempotencyTtlSeconds: 900,
      inFlightLeaseSeconds: 15,
    });

    const connectStartedAt = Date.now();
    await service.connect(); // must not throw, and must not hang for the outage
    const connectMs = Date.now() - connectStartedAt;
    expect(connectMs).toBeLessThan(5_000);

    expect(await withDeadline(service.claim(key('boot-during')), 1_000)).toEqual({
      state: 'unavailable',
    });

    await new Promise((resolve) => setTimeout(resolve, OUTAGE_MS));
    compose('start redis');

    const recoveredAfterMs = await pollUntilReachable(service, 15_000);
    expect(recoveredAfterMs).not.toBeNull();
    expect((await service.claim(key('boot-after'))).state).toBe('claimed');

    await service.onModuleDestroy();
  });
});
