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

    // Set only when this request is the one holding the claim, so the catch
    // below releases a key it actually took. A duplicate or an unavailable
    // Redis leaves this undefined and releases nothing.
    let claimedKey: string | undefined;

    if (idempotencyKey !== undefined && idempotencyKey.length > 0) {
      const claim = await this.idempotency.claim(idempotencyKey);

      if (claim === 'committed') {
        // Durably stored by an earlier attempt. This is now true by
        // construction: 'committed' is only written after the insert commits,
        // so reporting success does not tell the sink a lost batch arrived.
        this.metrics.increment('batches_duplicate_suppressed');
        return { received, stored: 0, skipped, duplicate: true };
      }

      if (claim === 'in_flight') {
        // Another attempt is inside its insert right now. Neither answer the
        // boolean key could give was safe: 'duplicate' would drop a batch that
        // may never land, and letting this request insert would store it
        // twice. Refuse with a retryable error and let the sink come back --
        // by then the holder has either committed or released the key.
        this.metrics.increment('batches_in_flight_rejected');
        throw new Error('another attempt for this batch is in flight');
      }

      if (claim === 'claimed') {
        // ONLY on 'claimed'. The 'unavailable' fail-open path must not set
        // this: claim() also returns 'unavailable' when the client is open but
        // the SET itself errored, and in that case another request may hold a
        // real claim on this key. Releasing it in our catch block would delete
        // a live claim we never took.
        claimedKey = idempotencyKey;
      }

      // 'unavailable' falls through and ingests without protection.
      // IdempotencyService.claim() already counted redis_unavailable.
    }

    if (skipped > 0) {
      this.metrics.increment('events_skipped_malformed', skipped);
    }

    try {
      const stored = await this.repository.insertEvents(valid);
      this.metrics.increment('events_stored', stored);
      // Only now is 'committed' true. Doing this before the insert would
      // recreate the defect this phase closes.
      if (claimedKey !== undefined) {
        await this.idempotency.markCommitted(claimedKey);
      }
      return { received, stored, skipped, duplicate: false };
    } catch (error) {
      this.metrics.increment('db_failures');
      // Undo the claim: this batch was NOT stored, so the sink's retry must be
      // a real retry rather than one suppressed as an already-stored
      // duplicate. Claiming stays BEFORE the insert -- that is what closes the
      // window where a retry arriving mid-insert would store the batch twice
      // -- and the key is only given back once the insert has definitively
      // failed. release() never throws, so it cannot mask the database error
      // being rethrown here as a 503.
      if (claimedKey !== undefined) {
        await this.idempotency.release(claimedKey);
      }
      throw error;
    }
  }
}
