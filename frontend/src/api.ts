export class ApiError extends Error {
  status: number;

  constructor(message: string, status: number) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
  }
}

export interface HealthResponse {
  status: string;
  dependencies: { database: boolean; redis: boolean };
  counters: {
    batches_received: number;
    batches_duplicate_suppressed: number;
    events_stored: number;
    events_skipped_malformed: number;
    db_failures: number;
    redis_unavailable: number;
  };
}

export interface EventRecord {
  device_id: number;
  timestamp: string;
  temperature: number;
  battery: number;
  latitude: number;
  longitude: number;
  event_type: string;
}

export interface EventsQueryResponse {
  events: EventRecord[];
  next_cursor: string | null;
}

export interface EventsQueryParams {
  deviceId: number;
  from?: string;
  to?: string;
  limit?: number;
  order?: 'asc' | 'desc';
  cursor?: string;
}

const BASE_URL = import.meta.env.VITE_API_BASE_URL ?? 'http://localhost:3000';

async function parseErrorMessage(response: Response): Promise<string> {
  try {
    const body: unknown = await response.json();
    if (body && typeof body === 'object' && 'message' in body && typeof (body as { message: unknown }).message === 'string') {
      return (body as { message: string }).message;
    }
  } catch {
    // fall through to the generic message below
  }
  return `Request failed with status ${response.status}`;
}

export async function getHealth(): Promise<HealthResponse> {
  const response = await fetch(`${BASE_URL}/v1/health`);
  if (!response.ok) {
    throw new ApiError(await parseErrorMessage(response), response.status);
  }
  return response.json() as Promise<HealthResponse>;
}

export async function queryEvents(params: EventsQueryParams): Promise<EventsQueryResponse> {
  const search = new URLSearchParams();
  search.set('device_id', String(params.deviceId));
  if (params.from) search.set('from', params.from);
  if (params.to) search.set('to', params.to);
  if (params.limit !== undefined) search.set('limit', String(params.limit));
  if (params.order) search.set('order', params.order);
  if (params.cursor) search.set('cursor', params.cursor);

  const response = await fetch(`${BASE_URL}/v1/events?${search.toString()}`);
  if (!response.ok) {
    throw new ApiError(await parseErrorMessage(response), response.status);
  }
  return response.json() as Promise<EventsQueryResponse>;
}
