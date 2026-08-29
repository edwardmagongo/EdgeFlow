import { Controller, Get } from '@nestjs/common';
import { MetricsService } from '../metrics/metrics.service';

@Controller('v1/health')
export class HealthController {
  constructor(private readonly metrics: MetricsService) {}

  // Counters plus liveness. Dependency reachability is added in Task 5, once
  // there is a Redis client to ask; reporting it before then would be a
  // hardcoded "ok", which is worse than not reporting it.
  @Get()
  health() {
    return { status: 'ok', counters: this.metrics.snapshot() };
  }
}
