import {
  BadRequestException,
  Body,
  Controller,
  Headers,
  HttpCode,
  Post,
  ServiceUnavailableException,
} from '@nestjs/common';
import { IngestService } from './ingest.service';

@Controller('v1/events')
export class IngestController {
  constructor(private readonly ingest: IngestService) {}

  @Post()
  @HttpCode(200)
  async post(
    @Body() body: unknown,
    @Headers('idempotency-key') idempotencyKey?: string,
  ) {
    // An absent or non-string body means the request was not NDJSON text at
    // all. Nothing can be salvaged, so a permanent 4xx is the right answer --
    // the only case in this endpoint where that is true.
    if (typeof body !== 'string' || body.trim().length === 0) {
      throw new BadRequestException('expected a non-empty application/x-ndjson body');
    }

    try {
      return await this.ingest.ingest(body, idempotencyKey);
    } catch {
      // 503, never 500 and never 4xx: the sink retries 5xx and 429, and drops
      // everything else permanently. The distinction is the batch's survival.
      throw new ServiceUnavailableException('storage unavailable');
    }
  }
}
