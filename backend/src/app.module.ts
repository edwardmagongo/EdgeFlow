import { Module } from '@nestjs/common';
import { EventsRepository } from './db/events.repository';
import { pgPoolProvider } from './db/pool';
import { HealthController } from './health/health.controller';
import { MetricsService } from './metrics/metrics.service';

@Module({
  controllers: [HealthController],
  providers: [MetricsService, pgPoolProvider, EventsRepository],
  exports: [MetricsService, EventsRepository],
})
export class AppModule {}
