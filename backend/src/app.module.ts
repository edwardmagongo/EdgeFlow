import { Module } from '@nestjs/common';
import { EventsRepository } from './db/events.repository';
import { pgPoolProvider } from './db/pool';
import { HealthController } from './health/health.controller';
import { IdempotencyService } from './idempotency/idempotency.service';
import { MetricsService } from './metrics/metrics.service';

@Module({
  controllers: [HealthController],
  providers: [MetricsService, pgPoolProvider, EventsRepository, IdempotencyService],
  exports: [MetricsService, EventsRepository, IdempotencyService],
})
export class AppModule {}
