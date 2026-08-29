import { INestApplication } from '@nestjs/common';
import { Test } from '@nestjs/testing';
import request from 'supertest';
import { Pool } from 'pg';
import { AppModule } from '../src/app.module';
import { EventsRepository } from '../src/db/events.repository';
import { PG_POOL } from '../src/db/pool';
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

describe('GET /v1/events', () => {
  let app: INestApplication;
  let pool: Pool;
  let repository: EventsRepository;

  beforeAll(async () => {
    const moduleRef = await Test.createTestingModule({ imports: [AppModule] }).compile();
    app = moduleRef.createNestApplication();
    await app.init();
    pool = app.get<Pool>(PG_POOL);
    repository = app.get<EventsRepository>(EventsRepository);
  });

  beforeEach(async () => {
    await pool.query('TRUNCATE events');
  });

  afterAll(async () => {
    await app.close();
  });

  it('rejects a request with no device_id', async () => {
    const res = await request(app.getHttpServer()).get('/v1/events');
    expect(res.status).toBe(400);
  });

  it('rejects a non-numeric device_id', async () => {
    const res = await request(app.getHttpServer()).get('/v1/events?device_id=abc');
    expect(res.status).toBe(400);
  });

  it('rejects an unparseable from', async () => {
    const res = await request(app.getHttpServer()).get('/v1/events?device_id=1&from=not-a-date');
    expect(res.status).toBe(400);
  });

  it('rejects a limit outside range', async () => {
    const res = await request(app.getHttpServer()).get('/v1/events?device_id=1&limit=0');
    expect(res.status).toBe(400);
  });

  it('rejects an undecodable cursor', async () => {
    const res = await request(app.getHttpServer()).get('/v1/events?device_id=1&cursor=garbage');
    expect(res.status).toBe(400);
  });

  it('rejects an invalid order', async () => {
    const res = await request(app.getHttpServer()).get('/v1/events?device_id=1&order=sideways');
    expect(res.status).toBe(400);
  });

  it('returns an empty page for a device with no events', async () => {
    const res = await request(app.getHttpServer()).get('/v1/events?device_id=999');
    expect(res.status).toBe(200);
    expect(res.body).toEqual({ events: [], next_cursor: null });
  });

  it('returns snake_case fields matching the ingest wire format', async () => {
    await repository.insertEvents([row(1, '2026-08-29T10:00:00Z')]);

    const res = await request(app.getHttpServer()).get('/v1/events?device_id=1');

    expect(res.status).toBe(200);
    expect(res.body.events).toHaveLength(1);
    expect(res.body.events[0]).toMatchObject({
      device_id: 1,
      timestamp: '2026-08-29T10:00:00.000Z',
      temperature: 21.5,
      battery: 90,
      latitude: 37.7749,
      longitude: -122.4194,
      event_type: 'telemetry',
    });
  });

  it('walks a full pagination chain, visiting every seeded row exactly once', async () => {
    const timestamps = Array.from({ length: 7 }, (_, i) => `2026-08-29T10:0${i}:00Z`);
    await repository.insertEvents(timestamps.map((t) => row(1, t)));

    const seen: string[] = [];
    let cursor: string | undefined;
    for (let page = 0; page < 10; page++) {
      const query = cursor ? `?device_id=1&limit=3&cursor=${encodeURIComponent(cursor)}` : '?device_id=1&limit=3';
      const res = await request(app.getHttpServer()).get(`/v1/events${query}`);
      expect(res.status).toBe(200);
      for (const event of res.body.events) seen.push(event.timestamp);
      if (res.body.next_cursor === null) break;
      cursor = res.body.next_cursor;
    }

    const expectedDescending = [...timestamps].reverse().map((t) => new Date(t).toISOString());
    expect(seen).toEqual(expectedDescending);
  });

  it('is not corrupted by a concurrent insert between two page fetches', async () => {
    // Newest-first (default order). e3 is the newest of the three seeded
    // rows; page 1 (limit 2) returns [e3, e2]. A new row NEWER than
    // everything already fetched is inserted before page 2 is requested --
    // this is exactly the insert position that corrupts OFFSET-based
    // pagination (it shifts every later row's offset by one, causing page 2
    // to re-return a row page 1 already returned). Keyset pagination must not
    // repeat e2, must not lose e1, and must not surface the new row on page 2
    // (it's newer than the cursor position, so it correctly belongs on a
    // fresh first page, not this one).
    await repository.insertEvents([
      row(1, '2026-08-29T10:00:00Z'), // e1, oldest
      row(1, '2026-08-29T10:01:00Z'), // e2
      row(1, '2026-08-29T10:02:00Z'), // e3, newest
    ]);

    const page1 = await request(app.getHttpServer()).get('/v1/events?device_id=1&limit=2');
    expect(page1.status).toBe(200);
    expect(page1.body.events.map((e: { timestamp: string }) => e.timestamp)).toEqual([
      '2026-08-29T10:02:00.000Z',
      '2026-08-29T10:01:00.000Z',
    ]);
    expect(page1.body.next_cursor).not.toBeNull();

    // Concurrent insert: newer than e3.
    await repository.insertEvents([row(1, '2026-08-29T10:03:00Z')]);

    const page2 = await request(app.getHttpServer()).get(
      `/v1/events?device_id=1&limit=2&cursor=${encodeURIComponent(page1.body.next_cursor)}`,
    );
    expect(page2.status).toBe(200);
    expect(page2.body.events.map((e: { timestamp: string }) => e.timestamp)).toEqual([
      '2026-08-29T10:00:00.000Z',
    ]);
    expect(page2.body.next_cursor).toBeNull();
  });

  it('respects from/to bounds end to end', async () => {
    await repository.insertEvents([
      row(1, '2026-08-29T09:00:00Z'),
      row(1, '2026-08-29T10:00:00Z'),
      row(1, '2026-08-29T11:00:00Z'),
    ]);

    const res = await request(app.getHttpServer()).get(
      '/v1/events?device_id=1&from=2026-08-29T09:30:00Z&to=2026-08-29T10:30:00Z',
    );

    expect(res.status).toBe(200);
    expect(res.body.events).toHaveLength(1);
    expect(res.body.events[0].timestamp).toBe('2026-08-29T10:00:00.000Z');
  });
});
