import { BadRequestException, Injectable } from '@nestjs/common';
import { EventQueryRow, EventsRepository } from '../db/events.repository';
import { Cursor, decodeCursor, encodeCursor } from './cursor';

export interface RawQueryParams {
  deviceId?: string;
  from?: string;
  to?: string;
  limit?: string;
  cursor?: string;
  order?: string;
}

export interface EventsQueryResult {
  events: EventQueryRow[];
  nextCursor: string | null;
}

const DEFAULT_LIMIT = 100;
const MAX_LIMIT = 1000;

@Injectable()
export class EventsQueryService {
  constructor(private readonly repository: EventsRepository) {}

  async query(params: RawQueryParams): Promise<EventsQueryResult> {
    const deviceId = this.parseDeviceId(params.deviceId);
    const from = this.parseOptionalDate(params.from, 'from');
    const to = this.parseOptionalDate(params.to, 'to');
    const limit = this.parseLimit(params.limit);
    const order = this.parseOrder(params.order);
    const cursor = this.parseCursor(params.cursor);

    const rows = await this.repository.queryEvents({ deviceId, from, to, cursor, limit, order });

    const nextCursor =
      rows.length === limit ? encodeCursor(this.cursorFor(rows[rows.length - 1])) : null;

    return { events: rows, nextCursor };
  }

  private parseDeviceId(value: string | undefined): number {
    if (value === undefined || value.trim() === '') {
      throw new BadRequestException('device_id is required');
    }
    const deviceId = Number(value);
    if (!Number.isFinite(deviceId) || !Number.isInteger(deviceId)) {
      throw new BadRequestException('device_id must be an integer');
    }
    return deviceId;
  }

  private parseOptionalDate(value: string | undefined, name: string): Date | undefined {
    if (value === undefined) return undefined;
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) {
      throw new BadRequestException(`${name} must be a valid ISO-8601 timestamp`);
    }
    return date;
  }

  private parseLimit(value: string | undefined): number {
    if (value === undefined) return DEFAULT_LIMIT;
    const limit = Number(value);
    if (!Number.isInteger(limit) || limit < 1 || limit > MAX_LIMIT) {
      throw new BadRequestException(`limit must be an integer between 1 and ${MAX_LIMIT}`);
    }
    return limit;
  }

  private parseOrder(value: string | undefined): 'asc' | 'desc' {
    if (value === undefined) return 'desc';
    if (value !== 'asc' && value !== 'desc') {
      throw new BadRequestException('order must be "asc" or "desc"');
    }
    return value;
  }

  private parseCursor(value: string | undefined): Cursor | undefined {
    if (value === undefined) return undefined;
    const decoded = decodeCursor(value);
    if (decoded === null) {
      throw new BadRequestException('cursor is invalid');
    }
    return decoded;
  }

  private cursorFor(row: EventQueryRow): Cursor {
    return { timestamp: row.timestamp.toISOString(), id: row.id };
  }
}
