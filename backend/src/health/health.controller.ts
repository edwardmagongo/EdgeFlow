import { Controller, Get, Inject } from '@nestjs/common';
import { Pool } from 'pg';
import { PG_POOL } from '../db/pool';
import { IdempotencyService } from '../idempotency/idempotency.service';
import { MetricsService } from '../metrics/metrics.service';

@Controller('v1/health')
export class HealthController {
  constructor(
    private readonly metrics: MetricsService,
    private readonly idempotency: IdempotencyService,
    @Inject(PG_POOL) private readonly pool: Pool,
  ) {}

  @Get()
  async health() {
    let database = false;
    try {
      await this.pool.query('SELECT 1');
      database = true;
    } catch {
      database = false;
    }

    const redis = await this.idempotency.isReachable();

    return {
      // Derived, not hardcoded. This previously read 'ok' with a dead database,
      // which is what made it useless as a health check and why the ALB checks
      // /v1/health/live instead. The dashboard never trusted it either --
      // HealthStrip derives its own state from `dependencies` -- so the field
      // was misleading rather than merely redundant.
      //
      // This stays a 200 either way: the endpoint is a diagnostic, and the
      // liveness check is what anything routing traffic should act on.
      status: database && redis ? 'ok' : 'degraded',
      dependencies: { database, redis },
      counters: this.metrics.snapshot(),
    };
  }

  // Deliberately touches nothing. The ALB health check calls this: with one
  // ECS task and one shared RDS instance, failing the check on a database
  // outage would deregister the only target and turn the backend's own
  // retryable 503s into CloudFront 502s, without routing around anything.
  @Get('live')
  live() {
    return { status: 'live' };
  }
}
