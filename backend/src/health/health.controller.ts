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

    return {
      status: 'ok',
      dependencies: { database, redis: await this.idempotency.isReachable() },
      counters: this.metrics.snapshot(),
    };
  }
}
