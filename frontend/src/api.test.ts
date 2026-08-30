import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError, getHealth, queryEvents } from './api';

const healthyBody = {
  status: 'ok',
  dependencies: { database: true, redis: true },
  counters: {
    batches_received: 1,
    batches_duplicate_suppressed: 0,
    batches_in_flight_rejected: 0,
    events_stored: 1,
    events_skipped_malformed: 0,
    db_failures: 0,
    redis_unavailable: 0,
  },
};

describe('getHealth', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('returns the parsed health response on success', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => healthyBody }),
    );

    const result = await getHealth();

    expect(result).toEqual(healthyBody);
  });

  it('calls the /v1/health endpoint', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => healthyBody });
    vi.stubGlobal('fetch', fetchMock);

    await getHealth();

    const url = new URL(fetchMock.mock.calls[0][0] as string);
    expect(url.pathname).toBe('/v1/health');
  });

  it('throws an ApiError carrying the backend message and status on a non-2xx response', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({ ok: false, status: 503, json: async () => ({ message: 'storage unavailable' }) }),
    );

    try {
      await getHealth();
      throw new Error('expected getHealth to throw');
    } catch (err) {
      expect(err).toBeInstanceOf(ApiError);
      expect((err as ApiError).message).toBe('storage unavailable');
      expect((err as ApiError).status).toBe(503);
    }
  });
});

describe('queryEvents', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('builds a query string with only the provided parameters', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => ({ events: [], next_cursor: null }) });
    vi.stubGlobal('fetch', fetchMock);

    await queryEvents({ deviceId: 42, limit: 50, order: 'asc' });

    const url = new URL(fetchMock.mock.calls[0][0] as string);
    expect(url.pathname).toBe('/v1/events');
    expect(url.searchParams.get('device_id')).toBe('42');
    expect(url.searchParams.get('limit')).toBe('50');
    expect(url.searchParams.get('order')).toBe('asc');
    expect(url.searchParams.has('from')).toBe(false);
    expect(url.searchParams.has('to')).toBe(false);
    expect(url.searchParams.has('cursor')).toBe(false);
  });

  it('includes from, to, and cursor when provided', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => ({ events: [], next_cursor: null }) });
    vi.stubGlobal('fetch', fetchMock);

    await queryEvents({
      deviceId: 1,
      from: '2026-08-29T00:00:00Z',
      to: '2026-08-30T00:00:00Z',
      cursor: 'abc123',
    });

    const url = new URL(fetchMock.mock.calls[0][0] as string);
    expect(url.searchParams.get('from')).toBe('2026-08-29T00:00:00Z');
    expect(url.searchParams.get('to')).toBe('2026-08-30T00:00:00Z');
    expect(url.searchParams.get('cursor')).toBe('abc123');
  });

  it('returns the parsed events response on success', async () => {
    const body = {
      events: [
        {
          device_id: 1,
          timestamp: '2026-08-29T10:00:00.000Z',
          temperature: 21.5,
          battery: 90,
          latitude: 37.7749,
          longitude: -122.4194,
          event_type: 'telemetry',
        },
      ],
      next_cursor: 'xyz',
    };
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => body }));

    const result = await queryEvents({ deviceId: 1 });

    expect(result).toEqual(body);
  });

  it('rejects with an ApiError carrying the backend message on a 400', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({ ok: false, status: 400, json: async () => ({ message: 'device_id is required' }) }),
    );

    try {
      await queryEvents({ deviceId: 1 });
      throw new Error('expected queryEvents to throw');
    } catch (err) {
      expect(err).toBeInstanceOf(ApiError);
      expect((err as ApiError).message).toBe('device_id is required');
      expect((err as ApiError).status).toBe(400);
    }
  });

  it('falls back to a generic message when the error body is not JSON', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({
        ok: false,
        status: 503,
        json: async () => {
          throw new Error('not json');
        },
      }),
    );

    try {
      await queryEvents({ deviceId: 1 });
      throw new Error('expected queryEvents to throw');
    } catch (err) {
      expect(err).toBeInstanceOf(ApiError);
      expect((err as ApiError).status).toBe(503);
      expect((err as ApiError).message).toBe('Request failed with status 503');
    }
  });
});
