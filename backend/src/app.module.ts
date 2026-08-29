import { Module } from '@nestjs/common';
import { EventsRepository } from './db/events.repository';
import { pgPoolProvider } from './db/pool';
import { HealthController } from './health/health.controller';
import { IdempotencyService } from './idempotency/idempotency.service';
import { IngestController } from './ingest/ingest.controller';
import { IngestService } from './ingest/ingest.service';
import { MetricsService } from './metrics/metrics.service';

@Module({
  controllers: [HealthController, IngestController],
  providers: [
    MetricsService,
    pgPoolProvider,
    EventsRepository,
    IdempotencyService,
    IngestService,
  ],
  exports: [MetricsService, EventsRepository, IdempotencyService],
})
export class AppModule {}
