import { Injectable } from '@nestjs/common';

// Mirrors the gateway's shutdown-line culture: every batch and every event is
// accounted for by exactly one counter, so nothing disappears silently.
export type CounterName =
  | 'batches_received'
  | 'batches_duplicate_suppressed'
  | 'events_stored'
  | 'events_skipped_malformed'
  | 'db_failures'
  | 'redis_unavailable';

const COUNTER_NAMES: CounterName[] = [
  'batches_received',
  'batches_duplicate_suppressed',
  'events_stored',
  'events_skipped_malformed',
  'db_failures',
  'redis_unavailable',
];

@Injectable()
export class MetricsService {
  private readonly counters = new Map<CounterName, number>(
    COUNTER_NAMES.map((name) => [name, 0]),
  );

  increment(name: CounterName, by = 1): void {
    this.counters.set(name, (this.counters.get(name) ?? 0) + by);
  }

  snapshot(): Record<CounterName, number> {
    return Object.fromEntries(this.counters) as Record<CounterName, number>;
  }
}
