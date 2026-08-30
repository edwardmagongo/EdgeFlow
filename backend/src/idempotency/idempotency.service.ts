import { Injectable, Logger, OnModuleDestroy, OnModuleInit, Optional } from '@nestjs/common';
import { randomUUID } from 'node:crypto';
import { createClient, RedisClientType } from 'redis';
import { loadConfig } from '../config';
import { MetricsService } from '../metrics/metrics.service';

// 'in_flight' means another attempt is inside its insert right now. It is
// deliberately NOT 'duplicate': answering duplicate would tell the sink the
// batch is stored when it may never be, which is exactly the data loss this
// phase closes.
export type ClaimState = 'claimed' | 'in_flight' | 'committed' | 'unavailable';

// A discriminated union rather than a bare string, because only ONE of the
// four outcomes carries a lease the caller owns. 'claimed' hands back the
// per-attempt token that markCommitted()/release() require; the other three
// have nothing to give back, and the type makes it impossible for a caller to
// finish a lease it never took.
export type ClaimResult =
  | { state: 'claimed'; token: string }
  | { state: 'in_flight' }
  | { state: 'committed' }
  | { state: 'unavailable' };

export interface IdempotencyOptions {
  redisUrl: string;
  idempotencyTtlSeconds: number;
  // Optional so existing callers and loadConfig() keep working; defaults to
  // IN_FLIGHT_LEASE_SECONDS below.
  inFlightLeaseSeconds?: number;
}

// The two states a key can hold. The in-flight state additionally carries the
// holder's token, as `inflight:<token>`, so an attempt can prove ownership
// before finishing a lease. 'committed' is terminal and needs no token: once
// the rows are durable, nothing may take the key back.
const IN_FLIGHT = 'inflight';
const COMMITTED = 'committed';

function inFlightValue(token: string): string {
  return `${IN_FLIGHT}:${token}`;
}

// How long a claim survives without being committed or released.
//
// This is a CRASH-RECOVERY bound, not a "the retry lands before the sink gives
// up" mechanism. The earlier derivation from the sink's ~20.7s retry budget
// (4 attempts x 5s timeout + 700-1050ms of backoff) assumed every attempt burns
// its full 5s timeout. It does not: only the FIRST attempt times out against a
// hung holder -- attempts 2-4 hit the in-flight path, which answers 503 in
// milliseconds. The sink therefore exhausts at roughly 5.7-6.1s
// (5s timeout + 0.7-1.05s of backoff) and records the batch under
// batches_dropped_exhausted, with about 9s still left on this lease. No lease
// longer than the ~1s of post-timeout backoff could make the batch land on a
// later attempt, and a lease that short would strip live inserts of their
// leases and cause double-insertion -- a far worse trade.
//
// What the lease actually buys: a holder that dies or hangs mid-insert does not
// own the key forever. Once it expires, a FRESH request carrying the same
// Idempotency-Key -- an operator replay, a future re-send path -- can claim and
// store the batch, instead of being told in_flight until the deduplication
// window (idempotencyTtlSeconds, default 900s) elapses.
//
// 15s is kept because it brackets the two real constraints. It is about three
// orders of magnitude above a measured insert (a 100-event batch commits in
// single-digit milliseconds at Phase 6's 23,195 events/sec), so a live holder
// never loses its lease under load; and it is far below the 900s deduplication
// window, so a dead holder's key does not squat. It also clears the sink's
// ~6.1s give-up point, which means lease expiry never races an in-flight retry
// sequence -- expiry only ever happens after the sink has already stopped.
//
// It stays a constant rather than an environment variable because it trades
// double-insertion against key-squatting, not throughput against latency;
// there is no operational reason to tune it and a tempting wrong way to.
const IN_FLIGHT_LEASE_SECONDS = 15;

// Compare-and-delete. Deleting unconditionally lets an attempt whose lease has
// already expired remove a lease a DIFFERENT attempt legitimately holds, which
// reopens the mid-insert loss window this phase exists to close, just past the
// lease boundary. A GET-then-DEL from the client would be the same race with
// extra steps, so the check and the delete are one Lua script and therefore one
// atomic Redis operation.
const RELEASE_IF_OWNER = `
if redis.call('GET', KEYS[1]) == ARGV[1] then
  return redis.call('DEL', KEYS[1])
end
return 0
`;

// Compare-and-set, for the same reason: an attempt that lost its lease must not
// promote the key to 'committed' on behalf of whoever holds it now. Doing so
// would report 200 duplicate:true for a batch the current holder may never
// store.
const COMMIT_IF_OWNER = `
if redis.call('GET', KEYS[1]) == ARGV[1] then
  redis.call('SET', KEYS[1], ARGV[2], 'EX', ARGV[3])
  return 1
end
return 0
`;

// How long connect() waits for a first successful connection before returning
// and leaving the retry loop running in the background. Boot must not block on
// Redis: the readiness guard below already answers 'unavailable' until the
// first connection lands, so a slow or absent Redis costs deduplication for a
// while rather than costing the service its startup.
const STARTUP_READY_WAIT_MS = 2000;

@Injectable()
export class IdempotencyService implements OnModuleInit, OnModuleDestroy {
  private client: RedisClientType | null = null;
  private readonly logger = new Logger('IdempotencyService');
  private disconnected = false;
  private hasEverConnected = false;
  private reconnectAttempts = 0;

  // `IdempotencyOptions` is an interface, so it has no runtime type for
  // Nest's DI to resolve: TypeScript's emitted design:paramtypes metadata
  // collapses it to `Object`, and Nest has no provider registered under that
  // token. `@Optional()` tells Nest to pass `undefined` instead of throwing
  // when it can't resolve this param, which lets the default value below
  // (loadConfig() from the real environment) take over -- exactly the
  // behaviour the brief's constructor signature intends when the service is
  // instantiated via the AppModule rather than `new`'d directly in tests.
  constructor(
    private readonly metrics: MetricsService,
    @Optional() private readonly options: IdempotencyOptions = loadConfig(),
  ) {}

  async onModuleInit(): Promise<void> {
    await this.connect();
  }

  async connect(): Promise<void> {
    const client = createClient({
      url: this.options.redisUrl,
      // Without this, node-redis buffers commands issued while the socket is
      // down and replays them when it returns -- so a claim() during an outage
      // does not fail, it HANGS for the length of the outage. That is strictly
      // worse than failing open: the ingest request blocks, the sink burns all
      // four attempts at the full 5s timeout, and the batch is dropped as
      // exhausted. Failing fast is what lets the fail-open path actually run.
      disableOfflineQueue: true,
      socket: {
        connectTimeout: 1000,
        // ONE regime, deliberately. An earlier version gave up after a couple
        // of tries when no connection had ever succeeded, on the theory that a
        // bad URL should fail fast at boot. That relocated the permanent
        // fail-open this phase exists to remove from "after a Redis restart"
        // to "Redis was not up yet at boot" -- the more common case, since a
        // compose or k8s start races the backend against Redis. Returning an
        // Error from here makes node-redis give up for the LIFE of the
        // process, so it must never be returned. The startup hang that
        // motivated the split is handled in connect() below by not awaiting
        // client.connect() to completion.
        //
        // Capped at 2s so a long outage is a slow poll rather than a tight
        // retry loop, which is what `reconnectStrategy: false` originally
        // guarded against.
        reconnectStrategy: (retries: number) => {
          this.reconnectAttempts += 1;
          return Math.min(50 * 2 ** Math.min(retries, 6), 2000);
        },
      },
    }) as RedisClientType;

    // node-redis emits 'error' on every failed reconnect attempt, so logging
    // each one would reproduce the flooding this used to avoid. Log the
    // transition instead: one line when the connection drops, one when it
    // returns. An unhandled 'error' event would still crash the process, so
    // this handler must exist regardless of whether it logs.
    client.on('error', () => {
      if (!this.disconnected) {
        this.disconnected = true;
        this.logger.warn(
          this.hasEverConnected
            ? 'redis connection lost; deduplication is failing open'
            : 'redis not reachable yet; deduplication is failing open until it connects',
        );
      }
    });
    client.on('ready', () => {
      const wasDisconnected = this.disconnected;
      this.disconnected = false;
      if (!this.hasEverConnected) {
        // "restored" would be a lie here: nothing was ever up to be restored.
        // A failed first attempt followed by a success reaches this branch,
        // and reading "connection restored" for a connection that never
        // existed sends an operator hunting for an outage that never happened.
        this.hasEverConnected = true;
        this.logger.log('redis connected; deduplication active');
      } else if (wasDisconnected) {
        this.logger.log('redis connection restored; deduplication resumed');
      }
    });

    this.client = client;

    // Deliberately NOT awaited to completion. With a delay-returning
    // reconnectStrategy (the only safe kind, see above) node-redis retries
    // internally and this promise never settles while Redis is down, so
    // awaiting it would hang startup for the length of the outage. Waiting a
    // bounded moment below keeps the common case -- Redis already up -- fully
    // connected by the time connect() returns, which is what callers and tests
    // expect, without making boot hostage to a dependency that is allowed to
    // be missing.
    client.connect().catch(() => undefined);
    await this.waitForReady(client, STARTUP_READY_WAIT_MS);

    if (!client.isReady) {
      this.logger.warn('redis unavailable at startup; will keep retrying');
    }
  }

  private waitForReady(client: RedisClientType, timeoutMs: number): Promise<void> {
    if (client.isReady) return Promise.resolve();
    return new Promise<void>((resolve) => {
      const finish = (): void => {
        clearTimeout(timer);
        client.off('ready', finish);
        resolve();
      };
      const timer = setTimeout(finish, timeoutMs);
      // The deadline must not be a reason for the process (or a test run) to
      // stay alive on its own.
      timer.unref?.();
      client.once('ready', finish);
    });
  }

  // `isReady`, NOT `isOpen`. node-redis keeps isOpen true for the whole
  // reconnect cycle -- it reflects "a client object with a socket", not "a
  // usable connection" -- so guarding on isOpen lets commands through during an
  // outage, where (before disableOfflineQueue) they queued and hung. isReady is
  // the only property that means the connection can actually serve a command.
  private ready(): boolean {
    return this.client !== null && this.client.isReady;
  }

  // One atomic command does both jobs: NX refuses to overwrite an existing
  // key, GET returns whatever was already there. The returned value IS the
  // state, so there is no read-then-write window for a concurrent retry to
  // slip through. Requires Redis >= 7.0 for NX and GET together; the compose
  // file pins redis:7.
  //
  // The value written is `inflight:<token>` with a token unique to this call,
  // which is what later lets release()/markCommitted() prove they still own the
  // lease they are finishing.
  async claim(key: string): Promise<ClaimResult> {
    if (!this.ready()) {
      this.metrics.increment('redis_unavailable');
      return { state: 'unavailable' };
    }
    const token = randomUUID();
    try {
      const previous = await this.client!.set(key, inFlightValue(token), {
        NX: true,
        GET: true,
        EX: this.options.inFlightLeaseSeconds ?? IN_FLIGHT_LEASE_SECONDS,
      });
      if (previous === null) {
        return { state: 'claimed', token };
      }
      if (previous === COMMITTED) {
        return { state: 'committed' };
      }
      // Any other value, including an inflight:<token> from another attempt:
      // treat as in flight. Refusing is the conservative answer -- it costs a
      // retry, where guessing wrong in the other direction costs the batch.
      return { state: 'in_flight' };
    } catch {
      this.metrics.increment('redis_unavailable');
      return { state: 'unavailable' };
    }
  }

  // Promotes a held lease to the durable deduplication window. Called ONLY
  // after the insert has committed, which is what makes a later 'committed'
  // answer -- and the 200 duplicate the endpoint returns for it -- actually
  // true.
  //
  // Conditional on `token` still being the value in the key. If this attempt's
  // lease expired and another attempt has since claimed the key, promoting it
  // would tell a third attempt "durably stored" about a batch the CURRENT
  // holder may still fail to store. In that case the write is skipped: the new
  // holder's own markCommitted is the one entitled to promote it.
  //
  // Never throws: the caller has already stored the batch successfully, and
  // failing the request now would make the sink retry data that is already
  // durable. If this fails or is refused, the lease simply expires and a later
  // retry may store the batch a second time; duplication over loss, and the
  // redis_unavailable counter records the failure case.
  async markCommitted(key: string, token: string): Promise<void> {
    if (!this.ready()) {
      this.metrics.increment('redis_unavailable');
      return;
    }
    try {
      await this.client!.eval(COMMIT_IF_OWNER, {
        keys: [key],
        arguments: [
          inFlightValue(token),
          COMMITTED,
          String(this.options.idempotencyTtlSeconds),
        ],
      });
    } catch {
      this.metrics.increment('redis_unavailable');
    }
  }

  // Deletes the key so a later claim() of it is not treated as a duplicate.
  // Used to undo a claim() when the insert it was guarding fails, so the
  // sink's retry is a genuine retry rather than one silently suppressed as
  // already-stored. Best-effort, like claim(): an absent or unreachable
  // client is tolerated rather than thrown -- the caller (IngestService)
  // must not let a failed release mask the original database error, so this
  // method never surfaces one of its own.
  //
  // Conditional on `token`, for the mirror-image reason markCommitted is: an
  // unconditional DEL past lease expiry would delete a lease another attempt
  // holds, letting a concurrent retry insert the same batch a second time --
  // or, worse, letting the real holder's later markCommitted resurrect a key
  // for rows that were never stored.
  async release(key: string, token: string): Promise<void> {
    if (!this.ready()) {
      return;
    }
    try {
      await this.client!.eval(RELEASE_IF_OWNER, {
        keys: [key],
        arguments: [inFlightValue(token)],
      });
    } catch {
      // Swallow. Worst case the in-flight lease simply expires on its own
      // within IN_FLIGHT_LEASE_SECONDS and the next retry claims it then -- a
      // delay, not a loss, and far better than surfacing this error in place
      // of the real 503.
    }
  }

  async isReachable(): Promise<boolean> {
    if (!this.ready()) return false;
    try {
      await this.client!.ping();
      return true;
    } catch {
      return false;
    }
  }

  // Test seam: asserting the TTL exists is how the plan proves keys expire
  // rather than accumulating without bound.
  async ttlForTesting(key: string): Promise<number> {
    if (this.client === null) return -1;
    return this.client.ttl(key);
  }

  // Test seam: how many times the reconnect strategy has been consulted on this
  // instance. Without it, "retries indefinitely with a capped backoff" can only
  // be asserted by its end result (recovery), which a single lucky first retry
  // also satisfies.
  reconnectAttemptsForTesting(): number {
    return this.reconnectAttempts;
  }

  async onModuleDestroy(): Promise<void> {
    const client = this.client;
    // Reset first, and unconditionally: a connect() after a destroy on the same
    // instance must start from a clean slate, or it inherits a stale
    // "already connected once" regime and logs transitions that did not happen.
    this.client = null;
    this.disconnected = false;
    this.hasEverConnected = false;
    this.reconnectAttempts = 0;

    if (client !== null) {
      if (client.isReady) {
        await client.quit().catch(() => undefined);
      } else {
        // A client that is not ready is still retrying in the background.
        // quit() needs a working connection to send QUIT on, so tearing this
        // one down needs disconnect(); awaiting it is what keeps the retry loop
        // from outliving the module and holding the Node event loop (or a test
        // run) open.
        await client.disconnect().catch(() => undefined);
      }
    }
  }
}
