import { Injectable, OnModuleDestroy, OnModuleInit, Optional } from '@nestjs/common';
import { createClient, RedisClientType } from 'redis';
import { loadConfig } from '../config';
import { MetricsService } from '../metrics/metrics.service';

export type ClaimResult = 'claimed' | 'duplicate' | 'unavailable';

export interface IdempotencyOptions {
  redisUrl: string;
  idempotencyTtlSeconds: number;
}

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

  // SET key 1 NX EX ttl. Returns 'claimed' when this caller won the key,
  // 'duplicate' when it already existed, and 'unavailable' when Redis could not
  // answer -- in which case the caller ingests anyway. See the fail-open note.
  async claim(key: string): Promise<ClaimResult> {
    if (this.client === null || !this.client.isOpen) {
      this.metrics.increment('redis_unavailable');
      return 'unavailable';
    }
    try {
      const result = await this.client.set(key, '1', {
        NX: true,
        EX: this.options.idempotencyTtlSeconds,
      });
      return result === null ? 'duplicate' : 'claimed';
    } catch {
      this.metrics.increment('redis_unavailable');
      return 'unavailable';
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
      // Swallow. Worst case the key simply lives out its TTL and the next
      // retry is suppressed as a duplicate instead of released -- survivable,
      // unlike surfacing this error in place of the real 503 cause.
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
