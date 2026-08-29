import { Cursor, decodeCursor, encodeCursor } from '../src/events-query/cursor';

describe('cursor encode/decode', () => {
  it('round-trips a cursor through encode then decode', () => {
    const cursor: Cursor = { timestamp: '2026-08-29T10:15:00.000Z', id: 42 };
    const encoded = encodeCursor(cursor);
    expect(decodeCursor(encoded)).toEqual(cursor);
  });

  it('returns null for a value that is not valid base64-encoded JSON', () => {
    expect(decodeCursor('not-valid-base64-json!!!')).toBeNull();
  });

  it('returns null for valid base64 that decodes to non-JSON', () => {
    const garbage = Buffer.from('not json at all', 'utf8').toString('base64');
    expect(decodeCursor(garbage)).toBeNull();
  });

  it('returns null when the decoded object is missing fields', () => {
    const missingId = Buffer.from(JSON.stringify({ timestamp: '2026-08-29T10:15:00.000Z' }), 'utf8').toString(
      'base64',
    );
    expect(decodeCursor(missingId)).toBeNull();

    const missingTimestamp = Buffer.from(JSON.stringify({ id: 42 }), 'utf8').toString('base64');
    expect(decodeCursor(missingTimestamp)).toBeNull();
  });

  it('returns null when id is not a finite number', () => {
    const nanId = Buffer.from(JSON.stringify({ timestamp: '2026-08-29T10:15:00.000Z', id: 'not-a-number' }), 'utf8').toString(
      'base64',
    );
    expect(decodeCursor(nanId)).toBeNull();
  });

  it('returns null when timestamp is not a parseable date', () => {
    const badDate = Buffer.from(JSON.stringify({ timestamp: 'not-a-date', id: 42 }), 'utf8').toString('base64');
    expect(decodeCursor(badDate)).toBeNull();
  });

  it('returns null when id is not an integer', () => {
    const fractionalId = Buffer.from(
      JSON.stringify({ timestamp: '2026-08-29T10:15:00.000Z', id: 1.5 }),
      'utf8',
    ).toString('base64');
    expect(decodeCursor(fractionalId)).toBeNull();
  });
});
