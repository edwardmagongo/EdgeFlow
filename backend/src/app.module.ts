import { Module } from '@nestjs/common';
import { EventsRepository } from './db/events.repository';
import { pgPoolProvider } from './db/pool';
import { EventsQueryController } from './events-query/events-query.controller';
import { EventsQueryService } from './events-query/events-query.service';
import { HealthController } from './health/health.controller';
import { IdempotencyService } from './idempotency/idempotency.service';
import { IngestController } from './ingest/ingest.controller';
import { IngestService } from './ingest/ingest.service';
import { MetricsService } from './metrics/metrics.service';

@Module({
  controllers: [HealthController, IngestController, EventsQueryController],
  providers: [
    MetricsService,
    pgPoolProvider,
    EventsRepository,
    IdempotencyService,
    IngestService,
    EventsQueryService,
  ],
  exports: [MetricsService, EventsRepository, IdempotencyService],
})
export class AppModule {}
