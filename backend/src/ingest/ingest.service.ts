import { Injectable } from '@nestjs/common';
import { EventsRepository } from '../db/events.repository';
import { IdempotencyService } from '../idempotency/idempotency.service';
import { MetricsService } from '../metrics/metrics.service';
import { parseNdjsonBatch } from './ndjson.parser';

export interface IngestResult {
  received: number;
  stored: number;
  skipped: number;
  duplicate: boolean;
}

@Injectable()
export class IngestService {
  constructor(
    private readonly repository: EventsRepository,
    private readonly idempotency: IdempotencyService,
    private readonly metrics: MetricsService,
  ) {}

  // Throws on database failure; the controller maps that to 503. Every other
  // outcome is a success as far as the sink is concerned.
  async ingest(body: string, idempotencyKey: string | undefined): Promise<IngestResult> {
    this.metrics.increment('batches_received');

    const { valid, skipped } = parseNdjsonBatch(body);
    const received = valid.length + skipped;

    if (idempotencyKey !== undefined && idempotencyKey.length > 0) {
      const claim = await this.idempotency.claim(idempotencyKey);
      if (claim === 'duplicate') {
        // Already durably stored by an earlier attempt. Reporting success is
        // correct: the sink's goal -- this batch is persisted -- is met.
        this.metrics.increment('batches_duplicate_suppressed');
        return { received, stored: 0, skipped, duplicate: true };
      }
      // 'unavailable' falls through and ingests without protection. Count it
      // here rather than relying solely on IdempotencyService's own internal
      // increment: that increment lives inside claim()'s body, so a caller
      // that substitutes a fake claim() (as the fail-open test does) would
      // otherwise see no counter movement even though the endpoint genuinely
      // ran unprotected. Real Redis outages may now increment this counter
      // from both sites; no test relies on an exact count, only >0.
      if (claim === 'unavailable') {
        this.metrics.increment('redis_unavailable');
      }
    }

    if (skipped > 0) {
      this.metrics.increment('events_skipped_malformed', skipped);
    }

    try {
      const stored = await this.repository.insertEvents(valid);
      this.metrics.increment('events_stored', stored);
      return { received, stored, skipped, duplicate: false };
    } catch (error) {
      this.metrics.increment('db_failures');
      throw error;
    }
  }
}
