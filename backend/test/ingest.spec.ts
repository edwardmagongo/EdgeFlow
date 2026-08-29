import { INestApplication } from '@nestjs/common';
import { Test } from '@nestjs/testing';
import * as express from 'express';
import { Pool } from 'pg';
import request from 'supertest';
import { AppModule } from '../src/app.module';
import { EventsRepository } from '../src/db/events.repository';
import { PG_POOL } from '../src/db/pool';
import { IdempotencyService } from '../src/idempotency/idempotency.service';

const NDJSON = 'application/x-ndjson';

function line(deviceId: number): string {
  return JSON.stringify({
    device_id: deviceId,
    timestamp: '2026-08-28T12:34:56Z',
    temperature: 21.5,
    battery: 90,
    latitude: 37.7749,
    longitude: -122.4194,
    event_type: 'telemetry',
  });
}

function body(count: number): string {
  return Array.from({ length: count }, (_, i) => line(i)).join('\n') + '\n';
}

function uniqueKey(): string {
  return `ingest-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

describe('POST /v1/events', () => {
  let app: INestApplication;
  let pool: Pool;

  beforeAll(async () => {
    const moduleRef = await Test.createTestingModule({ imports: [AppModule] }).compile();
    app = moduleRef.createNestApplication();
    app.use(express.text({ type: NDJSON, limit: '32mb' }));
    await app.init();
    pool = app.get<Pool>(PG_POOL);
  });

  beforeEach(async () => {
    await pool.query('TRUNCATE events');
  });

  afterAll(async () => {
    await app.close();
  });

  async function storedCount(): Promise<number> {
    const result = await pool.query('SELECT count(*)::int AS n FROM events');
    return result.rows[0].n;
  }

  it('stores a well-formed batch', async () => {
    const response = await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', uniqueKey())
      .send(body(10))
      .expect(200);

    expect(response.body).toEqual({ received: 10, stored: 10, skipped: 0, duplicate: false });
    expect(await storedCount()).toBe(10);
  });

  it('stores the valid lines and counts the bad one, WITHOUT a 4xx', async () => {
    // The most important test here. A 4xx would make the sink permanently drop
    // all ten events to reject the one malformed line.
    const mixed = `${line(1)}\nnot json\n${line(2)}\n`;
    const response = await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', uniqueKey())
      .send(mixed)
      .expect(200);

    expect(response.body.stored).toBe(2);
    expect(response.body.skipped).toBe(1);
    expect(await storedCount()).toBe(2);
  });

  it('returns 200 with stored:0 when every line is bad', async () => {
    const response = await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', uniqueKey())
      .send('not json\nalso not json\n')
      .expect(200);

    expect(response.body.stored).toBe(0);
    expect(response.body.skipped).toBe(2);
  });

  it('rejects an empty body with 400', async () => {
    await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', uniqueKey())
      .send('')
      .expect(400);
  });

  it('suppresses a replayed batch', async () => {
    const key = uniqueKey();
    const payload = body(5);

    const first = await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', key)
      .send(payload)
      .expect(200);
    expect(first.body).toMatchObject({ stored: 5, duplicate: false });

    const second = await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', key)
      .send(payload)
      .expect(200);
    expect(second.body).toMatchObject({ stored: 0, duplicate: true });

    expect(await storedCount()).toBe(5); // not 10
  });

  it('ingests without a key rather than refusing', async () => {
    // A client that sends no Idempotency-Key still gets its data stored; it
    // simply forfeits duplicate protection. Refusing would be a 4xx, which
    // destroys the batch.
    await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .send(body(3))
      .expect(200);

    expect(await storedCount()).toBe(3);
  });

  it('still stores rows when Redis is unavailable', async () => {
    // Fail open: a cache outage must not become telemetry loss. The service
    // returns 'unavailable' and the endpoint ingests anyway. This test mocks
    // the entire claim() method, which bypasses its real body -- including
    // the redis_unavailable increment that lives inside it -- so it proves
    // only IngestService's own fail-open storage behaviour, not the counter.
    // The counter itself is proven directly, against a genuinely unreachable
    // Redis instance, by "fails open when Redis is unreachable" in
    // idempotency.service.spec.ts.
    const idempotency = app.get(IdempotencyService);
    jest.spyOn(idempotency, 'claim').mockResolvedValueOnce('unavailable');

    const response = await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', uniqueKey())
      .send(body(6))
      .expect(200);

    expect(response.body.stored).toBe(6);
    expect(await storedCount()).toBe(6);
  });

  it('returns 503, not 500, when the database fails', async () => {
    // 503 is what the sink treats as retryable alongside 429. A 4xx here would
    // permanently drop the batch; this test is what keeps that from regressing.
    const repository = app.get(EventsRepository);
    const original = repository.insertEvents.bind(repository);
    jest
      .spyOn(repository, 'insertEvents')
      .mockRejectedValueOnce(new Error('connection terminated'));

    await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', uniqueKey())
      .send(body(4))
      .expect(503);

    jest.spyOn(repository, 'insertEvents').mockImplementation(original);
  });

  it('counts what it did on the health endpoint', async () => {
    await request(app.getHttpServer())
      .post('/v1/events')
      .set('Content-Type', NDJSON)
      .set('Idempotency-Key', uniqueKey())
      .send(`${line(1)}\nnot json\n`)
      .expect(200);

    const health = await request(app.getHttpServer()).get('/v1/health').expect(200);
    expect(health.body.counters.batches_received).toBeGreaterThan(0);
    expect(health.body.counters.events_stored).toBeGreaterThan(0);
    expect(health.body.counters.events_skipped_malformed).toBeGreaterThan(0);
  });
});
