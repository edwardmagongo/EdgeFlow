import { Pool } from 'pg';
import { EventQueryRow, EventsRepository, QueryEventsParams } from '../src/db/events.repository';
import { EventRow } from '../src/ingest/ndjson.parser';

function row(deviceId: number, timestamp: string): EventRow {
  return {
    deviceId,
    timestamp: new Date(timestamp),
    temperature: 21.5,
    battery: 90,
    latitude: 37.7749,
    longitude: -122.4194,
    eventType: 'telemetry',
  };
}

function baseParams(overrides: Partial<QueryEventsParams> = {}): QueryEventsParams {
  return { deviceId: 1, limit: 100, order: 'desc', ...overrides };
}

describe('EventsRepository.queryEvents', () => {
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

  it('returns only rows for the requested device, newest first by default', async () => {
    await repository.insertEvents([
      row(1, '2026-08-29T10:00:00Z'),
      row(1, '2026-08-29T10:01:00Z'),
      row(2, '2026-08-29T10:02:00Z'),
    ]);

    const rows = await repository.queryEvents(baseParams({ deviceId: 1 }));

    expect(rows).toHaveLength(2);
    expect(rows[0].timestamp.toISOString()).toBe('2026-08-29T10:01:00.000Z');
    expect(rows[1].timestamp.toISOString()).toBe('2026-08-29T10:00:00.000Z');
    expect(rows.every((r) => r.deviceId === 1)).toBe(true);
  });

  it('respects order=asc', async () => {
    await repository.insertEvents([row(1, '2026-08-29T10:00:00Z'), row(1, '2026-08-29T10:01:00Z')]);

    const rows = await repository.queryEvents(baseParams({ deviceId: 1, order: 'asc' }));

    expect(rows[0].timestamp.toISOString()).toBe('2026-08-29T10:00:00.000Z');
    expect(rows[1].timestamp.toISOString()).toBe('2026-08-29T10:01:00.000Z');
  });

  it('applies from/to bounds inclusively', async () => {
    await repository.insertEvents([
      row(1, '2026-08-29T09:59:59Z'),
      row(1, '2026-08-29T10:00:00Z'),
      row(1, '2026-08-29T10:01:00Z'),
      row(1, '2026-08-29T10:01:01Z'),
    ]);

    const rows = await repository.queryEvents(
      baseParams({
        deviceId: 1,
        order: 'asc',
        from: new Date('2026-08-29T10:00:00Z'),
        to: new Date('2026-08-29T10:01:00Z'),
      }),
    );

    expect(rows.map((r) => r.timestamp.toISOString())).toEqual([
      '2026-08-29T10:00:00.000Z',
      '2026-08-29T10:01:00.000Z',
    ]);
  });

  it('applies the cursor as an exclusive keyset bound', async () => {
    await repository.insertEvents([
      row(1, '2026-08-29T10:00:00Z'),
      row(1, '2026-08-29T10:01:00Z'),
      row(1, '2026-08-29T10:02:00Z'),
    ]);

    const firstPage = await repository.queryEvents(baseParams({ deviceId: 1, limit: 1 }));
    expect(firstPage).toHaveLength(1);
    expect(firstPage[0].timestamp.toISOString()).toBe('2026-08-29T10:02:00.000Z');

    const secondPage = await repository.queryEvents(
      baseParams({
        deviceId: 1,
        cursor: { timestamp: firstPage[0].timestamp.toISOString(), id: firstPage[0].id },
      }),
    );

    expect(secondPage).toHaveLength(2);
    expect(secondPage.map((r) => r.timestamp.toISOString())).toEqual([
      '2026-08-29T10:01:00.000Z',
      '2026-08-29T10:00:00.000Z',
    ]);
  });

  it('respects limit', async () => {
    await repository.insertEvents([
      row(1, '2026-08-29T10:00:00Z'),
      row(1, '2026-08-29T10:01:00Z'),
      row(1, '2026-08-29T10:02:00Z'),
    ]);

    const rows = await repository.queryEvents(baseParams({ deviceId: 1, limit: 2 }));
    expect(rows).toHaveLength(2);
  });

  it('returns an empty array for a device with no events', async () => {
    const rows = await repository.queryEvents(baseParams({ deviceId: 999 }));
    expect(rows).toEqual([]);
  });

  it('converts bigint device_id and id columns to JS numbers', async () => {
    await repository.insertEvents([row(1, '2026-08-29T10:00:00Z')]);
    const rows: EventQueryRow[] = await repository.queryEvents(baseParams({ deviceId: 1 }));
    expect(typeof rows[0].deviceId).toBe('number');
    expect(typeof rows[0].id).toBe('number');
  });

  // Regression test for the dynamic $N parameter-index construction: from, to,
  // and cursor are all optional and pushed onto `values` conditionally, so the
  // placeholder numbers must stay correct when all three are present at once.
  // A wrong index here would either bind the wrong value to the wrong
  // placeholder (silently returning a different -- often empty -- result) or
  // throw.
  //
  // In `desc` order both `to` and the cursor create *upper* bounds on
  // timestamp, so `to` only has an independently observable effect when it is
  // *more restrictive* than the cursor bound (i.e. `to` < cursor.timestamp).
  // Here `to` is 10:02 and the cursor is 10:04, so the cursor bound alone
  // (`timestamp < 10:04`) would let 10:03 through, but `to <= 10:02` excludes
  // it. If the `to` condition were silently dropped from the WHERE clause
  // (e.g. its value still pushed onto `values` but the corresponding
  // `conditions.push` omitted, shifting nothing but doing nothing), this test
  // would incorrectly include 10:03 in the result and fail -- which is the
  // discriminating power this test is meant to have.
  it('applies from, to, and cursor together correctly', async () => {
    await repository.insertEvents([
      row(1, '2026-08-29T09:00:00Z'), // before `from` -- excluded by from
      row(1, '2026-08-29T10:00:00Z'), // in range, older than cursor, within to -- included
      row(1, '2026-08-29T10:01:00Z'), // in range, older than cursor, within to -- included
      row(1, '2026-08-29T10:02:00Z'), // in range, older than cursor, AT the `to` bound -- included
      row(1, '2026-08-29T10:03:00Z'), // older than cursor but PAST `to` -- excluded by to, NOT by cursor
      row(1, '2026-08-29T10:04:00Z'), // this row IS the cursor -- excluded (exclusive bound)
      row(1, '2026-08-29T10:05:00Z'), // newer than cursor -- excluded by cursor
    ]);

    const all = await repository.queryEvents(baseParams({ deviceId: 1, order: 'asc', limit: 100 }));
    const cursorRow = all.find((r) => r.timestamp.toISOString() === '2026-08-29T10:04:00.000Z')!;

    const rows = await repository.queryEvents(
      baseParams({
        deviceId: 1,
        order: 'desc',
        from: new Date('2026-08-29T09:30:00Z'),
        to: new Date('2026-08-29T10:02:00Z'),
        cursor: { timestamp: cursorRow.timestamp.toISOString(), id: cursorRow.id },
      }),
    );

    // The cursor bound alone (timestamp < 10:04) would pass
    // 10:03, 10:02, 10:01, 10:00, 09:00. `to <= 10:02` additionally excludes
    // 10:03 -- a row the cursor bound alone would NOT have excluded, so this
    // specifically exercises `to`'s own binding. `from >= 09:30` additionally
    // excludes 09:00. The correct final result is exactly 10:02, 10:01,
    // 10:00 in descending order. If any placeholder were mis-numbered, or if
    // `to` were dropped entirely, this would instead include 10:03, be
    // empty, be otherwise wrong, or throw.
    expect(rows.map((r) => r.timestamp.toISOString())).toEqual([
      '2026-08-29T10:02:00.000Z',
      '2026-08-29T10:01:00.000Z',
      '2026-08-29T10:00:00.000Z',
    ]);
  });
});
