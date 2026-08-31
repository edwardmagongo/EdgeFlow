import { Pool } from 'pg';
import { EventsRepository } from '../src/db/events.repository';
import { EventRow } from '../src/ingest/ndjson.parser';

function row(deviceId: number): EventRow {
  return {
    deviceId,
    timestamp: new Date('2026-08-28T12:34:56Z'),
    temperature: 21.5,
    battery: 90,
    latitude: 37.7749,
    longitude: -122.4194,
    eventType: 'telemetry',
  };
}

describe('EventsRepository', () => {
  let pool: Pool;
  let repository: EventsRepository;

  beforeAll(() => {
    pool = new Pool({ connectionString: process.env.DATABASE_URL });
    repository = new EventsRepository(pool);
  });

  beforeEach(async () => {
    await pool.query('TRUNCATE events');
  });

  afterAll(async () => {
    await pool.end();
  });

  it('inserts every row and returns the count', async () => {
    const written = await repository.insertEvents([row(1), row(2), row(3)]);
    expect(written).toBe(3);

    const result = await pool.query('SELECT count(*)::int AS n FROM events');
    expect(result.rows[0].n).toBe(3);
  });

  it('round-trips every field', async () => {
    await repository.insertEvents([row(42)]);

    const result = await pool.query(
      'SELECT device_id, timestamp, temperature, battery, latitude, longitude, event_type FROM events',
    );
    const stored = result.rows[0];
    // bigint comes back as a string from pg by default.
    expect(String(stored.device_id)).toBe('42');
    expect(new Date(stored.timestamp).toISOString()).toBe('2026-08-28T12:34:56.000Z');
    expect(stored.temperature).toBeCloseTo(21.5);
    expect(stored.battery).toBe(90);
    expect(stored.latitude).toBeCloseTo(37.7749);
    expect(stored.longitude).toBeCloseTo(-122.4194);
    expect(stored.event_type).toBe('telemetry');
  });

  it('accepts many rows with the same device_id and timestamp', async () => {
    // The case a UNIQUE(device_id, timestamp) constraint would destroy: at 50
    // events/sec per device, fifty legitimate events share one second.
    const sameSecond = Array.from({ length: 50 }, () => row(7));
    const written = await repository.insertEvents(sameSecond);

    expect(written).toBe(50);
    const result = await pool.query('SELECT count(*)::int AS n FROM events');
    expect(result.rows[0].n).toBe(50);
  });

  it('writes a batch larger than one statement can bind', async () => {
    // 7 parameters per row against PostgreSQL's 65535-parameter ceiling means a
    // single statement tops out near 9,362 rows. --batch-size is operator-set,
    // so this is reachable in production, not a hypothetical.
    const many = Array.from({ length: 12000 }, (_, i) => row(i));
    const written = await repository.insertEvents(many);

    expect(written).toBe(12000);
    const result = await pool.query('SELECT count(*)::int AS n FROM events');
    expect(result.rows[0].n).toBe(12000);
  });

  it('writes nothing for an empty batch', async () => {
    expect(await repository.insertEvents([])).toBe(0);
  });

  it('leaves no partial batch behind when a row is bad', async () => {
    // The insert runs in a transaction, so a mid-batch failure must roll the
    // whole batch back rather than leave a prefix stored.
    const bad = [row(1), { ...row(2), eventType: null as unknown as string }, row(3)];
    await expect(repository.insertEvents(bad)).rejects.toThrow();

    const result = await pool.query('SELECT count(*)::int AS n FROM events');
    expect(result.rows[0].n).toBe(0);
  });
});

function rowsOf(count: number): EventRow[] {
  const rows: EventRow[] = [];
  for (let i = 0; i < count; i += 1) {
    rows.push({
      deviceId: 4242,
      timestamp: new Date(Date.UTC(2026, 7, 30, 0, 0, 0) + i * 1000),
      temperature: 21.5,
      battery: 90,
      latitude: 37.7749,
      longitude: -122.4194,
      eventType: 'telemetry',
    });
  }
  return rows;
}

// Records the SQL actually issued, so "no transaction was used" is asserted
// directly rather than inferred from timing. No database involved.
function recordingPool(): { pool: Pool; queries: string[] } {
  const queries: string[] = [];
  const client = {
    query: (text: unknown) => {
      queries.push(typeof text === 'string' ? text : String((text as { text: string }).text));
      return Promise.resolve({ rows: [] });
    },
    on: () => undefined,
    off: () => undefined,
    release: () => undefined,
  };
  const pool = { connect: () => Promise.resolve(client) } as unknown as Pool;
  return { pool, queries };
}

describe('insertEvents transaction shape', () => {
  it('issues no BEGIN or COMMIT for a batch that fits one statement', async () => {
    // The gateway's batch size is 100 and the chunk limit is 5000, so every
    // batch the pipeline actually sends takes this path. BEGIN and COMMIT are
    // two extra round trips on a statement that is already atomic by itself.
    const { pool, queries } = recordingPool();
    await new EventsRepository(pool).insertEvents(rowsOf(100));

    expect(queries).toHaveLength(1);
    expect(queries[0]).toContain('INSERT INTO events');
    expect(queries).not.toContain('BEGIN');
    expect(queries).not.toContain('COMMIT');
  });

  it('still wraps a multi-statement batch in a transaction', async () => {
    // Above the chunk limit the all-or-nothing guarantee is real and must
    // survive: two INSERTs cannot be atomic without one.
    const { pool, queries } = recordingPool();
    await new EventsRepository(pool).insertEvents(rowsOf(5001));

    expect(queries[0]).toBe('BEGIN');
    expect(queries[queries.length - 1]).toBe('COMMIT');
    expect(queries.filter((q) => q.includes('INSERT INTO events'))).toHaveLength(2);
  });

  it('takes the single-statement path at exactly the chunk limit', async () => {
    const { pool, queries } = recordingPool();
    await new EventsRepository(pool).insertEvents(rowsOf(5000));

    expect(queries).toHaveLength(1);
    expect(queries).not.toContain('BEGIN');
  });
});

describe('insertEvents multi-chunk atomicity', () => {
  let pool: Pool;

  beforeAll(() => {
    pool = new Pool({ connectionString: process.env.DATABASE_URL });
  });

  afterAll(async () => {
    await pool.end();
  });

  it('commits nothing when a LATER chunk fails after an earlier one executed', async () => {
    // 5001 rows = two statements. The failure is planted in the SECOND, so the
    // first has already executed inside the transaction when it fires. Without
    // the transaction the first 5000 rows would be durable and the batch would
    // be half-stored -- which the sink, seeing a 503, would then retry, storing
    // those 5000 a second time.
    const repository = new EventsRepository(pool); // this block's own pool, above
    await pool.query('DELETE FROM events WHERE device_id = 4242');

    const rows = rowsOf(5001);
    // NOT NULL on event_type is the constraint; the last row lands in chunk 2.
    (rows[5000] as unknown as { eventType: string | null }).eventType = null;

    await expect(repository.insertEvents(rows)).rejects.toThrow();

    const result = await pool.query('SELECT count(*) FROM events WHERE device_id = 4242');
    expect(Number(result.rows[0].count)).toBe(0);
  });
});

describe('insertEvents named statement', () => {
  it('names the insert statement so Postgres can reuse the plan', async () => {
    // A named query makes pg use the extended protocol and cache the parsed
    // plan per connection. The name must encode the row count, because the
    // statement text varies with it and reusing one name for two different
    // texts is an error.
    const names: string[] = [];
    const client = {
      query: (config: unknown) => {
        if (typeof config === 'object' && config !== null && 'name' in config) {
          names.push(String((config as { name: string }).name));
        }
        return Promise.resolve({ rows: [] });
      },
      on: () => undefined,
      off: () => undefined,
      release: () => undefined,
    };
    const pool = { connect: () => Promise.resolve(client) } as unknown as Pool;

    await new EventsRepository(pool).insertEvents(rowsOf(100));

    expect(names).toEqual(['insert_events_100']);
  });
});
