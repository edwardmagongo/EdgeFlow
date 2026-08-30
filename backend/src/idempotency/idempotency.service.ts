import { Injectable, OnModuleDestroy, OnModuleInit, Optional } from '@nestjs/common';
import { createClient, RedisClientType } from 'redis';
import { loadConfig } from '../config';
import { MetricsService } from '../metrics/metrics.service';

// 'in_flight' means another attempt is inside its insert right now. It is
// deliberately NOT 'duplicate': answering duplicate would tell the sink the
// batch is stored when it may never be, which is exactly the data loss this
// phase closes.
export type ClaimResult = 'claimed' | 'in_flight' | 'committed' | 'unavailable';

export interface IdempotencyOptions {
  redisUrl: string;
  idempotencyTtlSeconds: number;
  // Optional so existing callers and loadConfig() keep working; defaults to
  // IN_FLIGHT_LEASE_SECONDS below.
  inFlightLeaseSeconds?: number;
}

// The two values a key can hold.
const IN_FLIGHT = 'inflight';
const COMMITTED = 'committed';

// How long a claim survives without being committed or released. Bounded by
// the sink's retry budget -- 4 attempts x 5s timeout plus 100+200+400ms of
// backoff, about 20.7s -- so that a holder which dies mid-insert has its key
// expire while the sink is STILL retrying, and the batch lands on a later
// attempt instead of being dropped. Raising this above that budget silently
// reintroduces the loss this phase exists to close, which is why it is a
// constant here and not an environment variable.
const IN_FLIGHT_LEASE_SECONDS = 15;

@Injectable()
export class IdempotencyService implements OnModuleInit, OnModuleDestroy {
  private client: RedisClientType | null = null;

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
    // reconnectStrategy returning false stops node-redis retrying forever in
    // the background, which would otherwise flood logs during an outage. A
    // failed connection is not fatal: claim() degrades to 'unavailable'.
    const client = createClient({
      url: this.options.redisUrl,
      socket: { reconnectStrategy: false, connectTimeout: 1000 },
    }) as RedisClientType;

    // node-redis emits 'error' on connection loss; an unhandled 'error' event
    // would crash the process, so it is swallowed here and surfaced through the
    // redis_unavailable counter instead.
    client.on('error', () => undefined);

    try {
      await client.connect();
      this.client = client;
    } catch {
      this.client = null;
    }
  }

  // One atomic command does both jobs: NX refuses to overwrite an existing
  // key, GET returns whatever was already there. The returned value IS the
  // state, so there is no read-then-write window for a concurrent retry to
  // slip through. Requires Redis >= 7.0 for NX and GET together; the compose
  // file pins redis:7.
  async claim(key: string): Promise<ClaimResult> {
    if (this.client === null || !this.client.isOpen) {
      this.metrics.increment('redis_unavailable');
      return 'unavailable';
    }
    try {
      const previous = await this.client.set(key, IN_FLIGHT, {
        NX: true,
        GET: true,
        EX: this.options.inFlightLeaseSeconds ?? IN_FLIGHT_LEASE_SECONDS,
      });
      if (previous === null) {
        return 'claimed';
      }
      if (previous === COMMITTED) {
        return 'committed';
      }
      // Any other value, including IN_FLIGHT: treat as in flight. Refusing is
      // the conservative answer -- it costs a retry, where guessing wrong in
      // the other direction costs the batch.
      return 'in_flight';
    } catch {
      this.metrics.increment('redis_unavailable');
      return 'unavailable';
    }
  }

  // Promotes a held lease to the durable deduplication window. Called ONLY
  // after the insert has committed, which is what makes a later 'committed'
  // answer -- and the 200 duplicate the endpoint returns for it -- actually
  // true.
  //
  // Never throws: the caller has already stored the batch successfully, and
  // failing the request now would make the sink retry data that is already
  // durable. If this fails, the lease simply expires and a later retry may
  // store the batch a second time; duplication over loss, and the
  // redis_unavailable counter records it.
  async markCommitted(key: string): Promise<void> {
    if (this.client === null || !this.client.isOpen) {
      this.metrics.increment('redis_unavailable');
      return;
    }
    try {
      await this.client.set(key, COMMITTED, {
        EX: this.options.idempotencyTtlSeconds,
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
  async release(key: string): Promise<void> {
    if (this.client === null || !this.client.isOpen) {
      return;
    }
    try {
      await this.client.del(key);
    } catch {
      // Swallow. Worst case the in-flight lease simply expires on its own
      // within IN_FLIGHT_LEASE_SECONDS and the next retry claims it then -- a
      // delay, not a loss, and far better than surfacing this error in place
      // of the real 503.
    }
  }

  async isReachable(): Promise<boolean> {
    if (this.client === null || !this.client.isOpen) return false;
    try {
      await this.client.ping();
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

  async onModuleDestroy(): Promise<void> {
    if (this.client !== null && this.client.isOpen) {
      await this.client.quit().catch(() => undefined);
    }
    this.client = null;
  }
}
