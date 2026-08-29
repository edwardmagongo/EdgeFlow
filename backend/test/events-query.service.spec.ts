import { BadRequestException } from '@nestjs/common';
import { EventsRepository } from '../src/db/events.repository';
import { EventsQueryService } from '../src/events-query/events-query.service';
import { encodeCursor } from '../src/events-query/cursor';

function makeRepository(rows: unknown[] = []) {
  return {
    queryEvents: jest.fn().mockResolvedValue(rows),
  } as unknown as EventsRepository;
}

describe('EventsQueryService', () => {
  it('rejects a missing device_id', async () => {
    const service = new EventsQueryService(makeRepository());
    await expect(service.query({})).rejects.toBeInstanceOf(BadRequestException);
  });

  it('rejects a non-numeric device_id', async () => {
    const service = new EventsQueryService(makeRepository());
    await expect(service.query({ deviceId: 'abc' })).rejects.toBeInstanceOf(BadRequestException);
  });

  it('rejects a device_id that is an unsafely-large integer', async () => {
    const service = new EventsQueryService(makeRepository());
    await expect(service.query({ deviceId: '1e30' })).rejects.toBeInstanceOf(BadRequestException);
  });

  it('rejects an unparseable from', async () => {
    const service = new EventsQueryService(makeRepository());
    await expect(service.query({ deviceId: '1', from: 'not-a-date' })).rejects.toBeInstanceOf(BadRequestException);
  });

  it('rejects an unparseable to', async () => {
    const service = new EventsQueryService(makeRepository());
    await expect(service.query({ deviceId: '1', to: 'not-a-date' })).rejects.toBeInstanceOf(BadRequestException);
  });

  it('rejects a limit outside 1-1000', async () => {
    const service = new EventsQueryService(makeRepository());
    await expect(service.query({ deviceId: '1', limit: '0' })).rejects.toBeInstanceOf(BadRequestException);
    await expect(service.query({ deviceId: '1', limit: '1001' })).rejects.toBeInstanceOf(BadRequestException);
    await expect(service.query({ deviceId: '1', limit: 'abc' })).rejects.toBeInstanceOf(BadRequestException);
  });

  it('rejects an order that is neither asc nor desc', async () => {
    const service = new EventsQueryService(makeRepository());
    await expect(service.query({ deviceId: '1', order: 'sideways' })).rejects.toBeInstanceOf(BadRequestException);
  });

  it('rejects an undecodable cursor', async () => {
    const service = new EventsQueryService(makeRepository());
    await expect(service.query({ deviceId: '1', cursor: 'garbage' })).rejects.toBeInstanceOf(BadRequestException);
  });

  it('defaults limit to 100 and order to desc', async () => {
    const repository = makeRepository([]);
    const service = new EventsQueryService(repository);

    await service.query({ deviceId: '1' });

    expect(repository.queryEvents).toHaveBeenCalledWith(
      expect.objectContaining({ deviceId: 1, limit: 100, order: 'desc' }),
    );
  });

  it('passes a decoded cursor through to the repository', async () => {
    const repository = makeRepository([]);
    const service = new EventsQueryService(repository);
    const cursor = { timestamp: '2026-08-29T10:00:00.000Z', id: 5 };

    await service.query({ deviceId: '1', cursor: encodeCursor(cursor) });

    expect(repository.queryEvents).toHaveBeenCalledWith(expect.objectContaining({ cursor }));
  });

  it('returns next_cursor as null when fewer rows than the limit come back', async () => {
    const repository = makeRepository([
      { deviceId: 1, timestamp: new Date('2026-08-29T10:00:00Z'), id: 1, temperature: 1, battery: 1, latitude: 1, longitude: 1, eventType: 't' },
    ]);
    const service = new EventsQueryService(repository);

    const result = await service.query({ deviceId: '1', limit: '2' });

    expect(result.nextCursor).toBeNull();
  });

  it('returns a next_cursor built from the last row when the page is full', async () => {
    const rows = [
      { deviceId: 1, timestamp: new Date('2026-08-29T10:01:00Z'), id: 2, temperature: 1, battery: 1, latitude: 1, longitude: 1, eventType: 't' },
      { deviceId: 1, timestamp: new Date('2026-08-29T10:00:00Z'), id: 1, temperature: 1, battery: 1, latitude: 1, longitude: 1, eventType: 't' },
    ];
    const repository = makeRepository(rows);
    const service = new EventsQueryService(repository);

    const result = await service.query({ deviceId: '1', limit: '2' });

    expect(result.nextCursor).not.toBeNull();
    expect(result.events).toEqual(rows);
  });
});
