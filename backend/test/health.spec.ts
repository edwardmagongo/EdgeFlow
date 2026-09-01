import { INestApplication } from '@nestjs/common';
import { Test } from '@nestjs/testing';
import request from 'supertest';
import { AppModule } from '../src/app.module';

describe('GET /v1/health', () => {
  let app: INestApplication;

  beforeAll(async () => {
    const moduleRef = await Test.createTestingModule({
      imports: [AppModule],
    }).compile();
    app = moduleRef.createNestApplication();
    await app.init();
  });

  afterAll(async () => {
    await app.close();
  });

  it('reports ok, dependency reachability, and every counter at zero', async () => {
    const response = await request(app.getHttpServer()).get('/v1/health').expect(200);

    expect(response.body.status).toBe('ok');
    expect(response.body.dependencies).toEqual({ database: true, redis: true });
    expect(response.body.counters).toEqual({
      batches_received: 0,
      batches_duplicate_suppressed: 0,
      batches_in_flight_rejected: 0,
      events_stored: 0,
      events_skipped_malformed: 0,
      db_failures: 0,
      redis_unavailable: 0,
    });
  });
});

describe('GET /v1/health/live', () => {
  let app: INestApplication;

  beforeAll(async () => {
    const moduleRef = await Test.createTestingModule({
      imports: [AppModule],
    }).compile();
    app = moduleRef.createNestApplication();
    await app.init();
  });

  afterAll(async () => {
    await app.close();
  });

  // The ALB target group points here, not at /v1/health. /v1/health answers 200
  // even when its dependencies are down, so it would call a task with a dead
  // database healthy. This endpoint reports only that the process is up and
  // serving, which is the one question a load balancer with a single task and a
  // shared database can act on.
  it('reports liveness without touching any dependency', async () => {
    const response = await request(app.getHttpServer()).get('/v1/health/live').expect(200);

    expect(response.body).toEqual({ status: 'live' });
  });
});
