import { Controller, Get, HttpException, Query, ServiceUnavailableException } from '@nestjs/common';
import { EventsQueryResult, EventsQueryService } from './events-query.service';

@Controller('v1/events')
export class EventsQueryController {
  constructor(private readonly eventsQuery: EventsQueryService) {}

  @Get()
  async get(
    @Query('device_id') deviceId?: string,
    @Query('from') from?: string,
    @Query('to') to?: string,
    @Query('limit') limit?: string,
    @Query('cursor') cursor?: string,
    @Query('order') order?: string,
  ) {
    let result: EventsQueryResult;
    try {
      result = await this.eventsQuery.query({ deviceId, from, to, limit, cursor, order });
    } catch (err) {
      // Validation failures (e.g. BadRequestException) are client errors and
      // must surface as-is. Only a genuine downstream failure -- a Postgres
      // connection error propagating up through the repository -- is a
      // transient failure, matching the ingest endpoint's existing 503
      // convention.
      if (err instanceof HttpException) {
        throw err;
      }
      throw new ServiceUnavailableException('storage unavailable');
    }

    return {
      events: result.events.map((e) => ({
        device_id: e.deviceId,
        timestamp: e.timestamp.toISOString(),
        temperature: e.temperature,
        battery: e.battery,
        latitude: e.latitude,
        longitude: e.longitude,
        event_type: e.eventType,
      })),
      next_cursor: result.nextCursor,
    };
  }
}
