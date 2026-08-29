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
