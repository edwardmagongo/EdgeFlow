import { Inject, Injectable } from '@nestjs/common';
import { Pool } from 'pg';
import { EventRow } from '../ingest/ndjson.parser';
import { PG_POOL } from './pool';

const COLUMNS_PER_ROW = 7;
// PostgreSQL binds at most 65535 parameters per statement. At 7 per row that is
// about 9,362 rows; 5,000 keeps a wide margin without making the chunk count
// large for the 100-row batches the gateway actually sends.
const MAX_ROWS_PER_STATEMENT = 5000;

function chunk<T>(items: T[], size: number): T[][] {
  const chunks: T[][] = [];
  for (let i = 0; i < items.length; i += size) {
    chunks.push(items.slice(i, i + size));
  }
  return chunks;
}

@Injectable()
export class EventsRepository {
  constructor(@Inject(PG_POOL) private readonly pool: Pool) {}

  // Writes every row, or none. Throws on database failure; the caller maps that
  // to 503 so the sink retries rather than dropping the batch.
  async insertEvents(rows: EventRow[]): Promise<number> {
    if (rows.length === 0) return 0;

    const client = await this.pool.connect();
    try {
      await client.query('BEGIN');
      for (const group of chunk(rows, MAX_ROWS_PER_STATEMENT)) {
        await this.insertChunk(client, group);
      }
      await client.query('COMMIT');
      return rows.length;
    } catch (error) {
      // Rollback is itself best-effort: if the connection is already gone the
      // original error is the one worth surfacing.
      await client.query('ROLLBACK').catch(() => undefined);
      throw error;
    } finally {
      client.release();
    }
  }

  private async insertChunk(
    client: { query: (text: string, values: unknown[]) => Promise<unknown> },
    rows: EventRow[],
  ): Promise<void> {
    const values: unknown[] = [];
    const tuples: string[] = [];

    rows.forEach((row, index) => {
      const base = index * COLUMNS_PER_ROW;
      tuples.push(
        `($${base + 1}, $${base + 2}, $${base + 3}, $${base + 4}, $${base + 5}, $${base + 6}, $${base + 7})`,
      );
      values.push(
        row.deviceId,
        row.timestamp,
        row.temperature,
        row.battery,
        row.latitude,
        row.longitude,
        row.eventType,
      );
    });

    // Parameterised throughout: the values are device-supplied and must never
    // be interpolated into SQL.
    await client.query(
      'INSERT INTO events (device_id, timestamp, temperature, battery, latitude, longitude, event_type) VALUES ' +
        tuples.join(', '),
      values,
    );
  }
}
