import { Inject, Injectable, Logger } from '@nestjs/common';
import { Pool } from 'pg';
import { Cursor } from '../events-query/cursor';
import { EventRow } from '../ingest/ndjson.parser';
import { PG_POOL } from './pool';

const COLUMNS_PER_ROW = 7;
// PostgreSQL binds at most 65535 parameters per statement. At 7 per row that is
// about 9,362 rows; 5,000 keeps a wide margin without making the chunk count
// large for the 100-row batches the gateway actually sends.
const MAX_ROWS_PER_STATEMENT = 5000;

export interface EventQueryRow {
  deviceId: number;
  timestamp: Date;
  temperature: number;
  battery: number;
  latitude: number;
  longitude: number;
  eventType: string;
  id: number;
}

export interface QueryEventsParams {
  deviceId: number;
  from?: Date;
  to?: Date;
  cursor?: Cursor;
  limit: number;
  order: 'asc' | 'desc';
}

function chunk<T>(items: T[], size: number): T[][] {
  const chunks: T[][] = [];
  for (let i = 0; i < items.length; i += size) {
    chunks.push(items.slice(i, i + size));
  }
  return chunks;
}

@Injectable()
export class EventsRepository {
  private readonly logger = new Logger('EventsRepository');

  constructor(@Inject(PG_POOL) private readonly pool: Pool) {}

  // Writes every row, or none. Throws on database failure; the caller maps that
  // to 503 so the sink retries rather than dropping the batch.
  async insertEvents(rows: EventRow[]): Promise<number> {
    if (rows.length === 0) return 0;

    const client = await this.pool.connect();

    // A CHECKED-OUT client emits 'error' on itself, not on the pool: while a
    // client is lent out, pg removes the pool's own listener from it. So the
    // pool-level handler in pool.ts does NOT cover this window. If Postgres
    // terminates the connection mid-transaction -- `docker compose stop
    // postgres` produces exactly this, error 57P01 -- the Client emits 'error'
    // with no listener attached, which Node escalates to an uncaught exception
    // and the whole process dies. The in-flight query rejects separately and is
    // handled below; this listener only exists so the asynchronous socket-level
    // error cannot take the service down with it.
    let connectionFailed = false;
    const onClientError = (error: Error): void => {
      connectionFailed = true;
      this.logger.error(`connection lost mid-transaction: ${error.message}`);
    };
    client.on('error', onClientError);

    try {
      const groups = chunk(rows, MAX_ROWS_PER_STATEMENT);

      // A single INSERT is already atomic, so wrapping one in BEGIN/COMMIT buys
      // nothing and costs two round trips on a critical path the gateway walks
      // one request at a time. The transaction is kept for the multi-statement
      // case, where all-or-nothing genuinely needs it -- a partial batch would
      // be re-sent by the sink after its 503 and stored twice.
      if (groups.length === 1) {
        await this.insertChunk(client, groups[0]);
        return rows.length;
      }

      await client.query('BEGIN');
      for (const group of groups) {
        await this.insertChunk(client, group);
      }
      await client.query('COMMIT');
      return rows.length;
    } catch (error) {
      // Rollback is itself best-effort: if the connection is already gone the
      // original error is the one worth surfacing. Harmless on the
      // single-statement path, where there is no open transaction to undo.
      await client.query('ROLLBACK').catch(() => undefined);
      throw error;
    } finally {
      client.off('error', onClientError);
      // A client whose connection broke must be destroyed rather than returned
      // to the pool, or the pool hands the dead socket to the next caller.
      client.release(connectionFailed ? true : undefined);
    }
  }

  // Keyset pagination: the cursor bounds (timestamp, id) directly rather than
  // an OFFSET, so a concurrent insert anywhere in the table cannot cause an
  // already-returned row to repeat or a not-yet-returned row to be skipped --
  // see docs/superpowers/specs/2026-08-29-phase7-events-query-api-design.md.
  // Scope: this guarantee covers rows already fetched or present at query
  // time. A row inserted mid-pagination with a timestamp behind the reader's
  // current cursor position is not guaranteed to appear on a later page --
  // see the Goals section of the design spec for the precise statement.
  async queryEvents(params: QueryEventsParams): Promise<EventQueryRow[]> {
    const { deviceId, from, to, cursor, limit, order } = params;
    const comparisonOperator = order === 'desc' ? '<' : '>';
    const direction = order.toUpperCase();

    const conditions: string[] = ['device_id = $1'];
    const values: unknown[] = [deviceId];

    if (from) {
      values.push(from);
      conditions.push(`timestamp >= $${values.length}`);
    }
    if (to) {
      values.push(to);
      conditions.push(`timestamp <= $${values.length}`);
    }
    if (cursor) {
      values.push(new Date(cursor.timestamp), cursor.id);
      const tsParam = values.length - 1;
      const idParam = values.length;
      conditions.push(`(timestamp, id) ${comparisonOperator} ($${tsParam}, $${idParam})`);
    }

    values.push(limit);
    const limitParam = values.length;

    const result = await this.pool.query(
      `SELECT device_id, timestamp, temperature, battery, latitude, longitude, event_type, id
       FROM events
       WHERE ${conditions.join(' AND ')}
       ORDER BY timestamp ${direction}, id ${direction}
       LIMIT $${limitParam}`,
      values,
    );

    // device_id and id are bigint columns; pg returns bigint as a string by
    // default to avoid precision loss outside Number.MAX_SAFE_INTEGER. This
    // project's device IDs and row counts are always far below that range, so
    // converting back to a JS number here is deliberate, not a bug.
    return result.rows.map((r) => ({
      deviceId: Number(r.device_id),
      timestamp: r.timestamp,
      temperature: r.temperature,
      battery: r.battery,
      latitude: r.latitude,
      longitude: r.longitude,
      eventType: r.event_type,
      id: Number(r.id),
    }));
  }

  private async insertChunk(
    client: { query: (config: unknown) => Promise<unknown> },
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
    //
    // Named so Postgres parses and plans this once per connection instead of
    // on every request. The name carries the row count because the text does:
    // pg treats a name bound to two different texts as an error.
    await client.query({
      name: `insert_events_${rows.length}`,
      text:
        'INSERT INTO events (device_id, timestamp, temperature, battery, latitude, longitude, event_type) VALUES ' +
        tuples.join(', '),
      values,
    });
  }
}
