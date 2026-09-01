import { INestApplication } from '@nestjs/common';
import { Test } from '@nestjs/testing';
import { Pool } from 'pg';
import request from 'supertest';
import { AppModule } from '../src/app.module';
import { HealthController } from '../src/health/health.controller';
import { IdempotencyService } from '../src/idempotency/idempotency.service';
import { MetricsService } from '../src/metrics/metrics.service';

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

// status was hardcoded to 'ok' and said 'ok' with a dead database, which is
// what made it useless as a health check and forced /v1/health/live to exist.
// The dashboard never trusted it -- HealthStrip derives its own state from
// `dependencies` -- so the field was actively misleading rather than merely
// redundant. These construct the controller directly: the point is the mapping
// from dependency state to status, not the HTTP plumbing the block above covers.
describe('GET /v1/health status field', () => {
  function controllerWith(databaseUp: boolean, redisUp: boolean): HealthController {
    const pool = {
      query: async () => {
        if (!databaseUp) throw new Error('connect ECONNREFUSED');
        return { rows: [{ n: 1 }] };
      },
    } as unknown as Pool;

    const idempotency = {
      isReachable: async () => redisUp,
    } as unknown as IdempotencyService;

    return new HealthController(new MetricsService(), idempotency, pool);
  }

  it('reports ok when every dependency is reachable', async () => {
    const body = await controllerWith(true, true).health();

    expect(body.dependencies).toEqual({ database: true, redis: true });
    expect(body.status).toBe('ok');
  });

  it('reports degraded when the database is unreachable', async () => {
    const body = await controllerWith(false, true).health();

    expect(body.dependencies.database).toBe(false);
    expect(body.status).toBe('degraded');
  });

  it('reports degraded when redis is unreachable', async () => {
    const body = await controllerWith(true, false).health();

    expect(body.dependencies.redis).toBe(false);
    expect(body.status).toBe('degraded');
  });
});
