import { Module } from '@nestjs/common';
import { HealthController } from './health/health.controller';
import { MetricsService } from './metrics/metrics.service';

@Module({
  controllers: [HealthController],
  providers: [MetricsService],
  exports: [MetricsService],
})
export class AppModule {}
