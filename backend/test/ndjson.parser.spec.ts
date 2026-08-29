import { parseNdjsonBatch } from '../src/ingest/ndjson.parser';

const validLine = JSON.stringify({
  device_id: 42,
  timestamp: '2026-08-28T12:34:56Z',
  temperature: 21.5,
  battery: 90,
  latitude: 37.7749,
  longitude: -122.4194,
  event_type: 'telemetry',
});

describe('parseNdjsonBatch', () => {
  it('parses a well-formed batch', () => {
    const result = parseNdjsonBatch(`${validLine}\n${validLine}\n`);

    expect(result.skipped).toBe(0);
    expect(result.valid).toHaveLength(2);
    expect(result.valid[0].deviceId).toBe(42);
    expect(result.valid[0].eventType).toBe('telemetry');
    expect(result.valid[0].timestamp.toISOString()).toBe('2026-08-28T12:34:56.000Z');
  });

  it('ignores blank lines rather than counting them as malformed', () => {
    // FileSink terminates every line including the last, so a trailing newline
    // is normal output, not a defect.
    const result = parseNdjsonBatch(`${validLine}\n\n`);

    expect(result.valid).toHaveLength(1);
    expect(result.skipped).toBe(0);
  });

  it('keeps valid lines and skips the invalid one', () => {
    const result = parseNdjsonBatch(`${validLine}\nnot json\n${validLine}\n`);

    expect(result.valid).toHaveLength(2);
    expect(result.skipped).toBe(1);
  });

  it('skips a line missing a required field', () => {
    const missing = JSON.stringify({ device_id: 1, timestamp: '2026-08-28T12:34:56Z' });
    const result = parseNdjsonBatch(`${missing}\n`);

    expect(result.valid).toHaveLength(0);
    expect(result.skipped).toBe(1);
  });

  it('skips a line whose field has the wrong type', () => {
    const wrongType = JSON.stringify({
      device_id: 42,
      timestamp: '2026-08-28T12:34:56Z',
      temperature: 'warm',
      battery: 90,
      latitude: 0,
      longitude: 0,
      event_type: 'telemetry',
    });
    const result = parseNdjsonBatch(`${wrongType}\n`);

    expect(result.valid).toHaveLength(0);
    expect(result.skipped).toBe(1);
  });

  it('skips non-finite numbers', () => {
    // JSON.parse('1e400') yields Infinity rather than throwing. Stored and
    // re-serialised it becomes null and stops being parseable -- the same
    // hazard core/src/event.cpp guards against.
    const overflow = '{"device_id":1,"timestamp":"2026-08-28T12:34:56Z",' +
      '"temperature":1e400,"battery":90,"latitude":0,"longitude":0,"event_type":"t"}';
    const result = parseNdjsonBatch(`${overflow}\n`);

    expect(result.valid).toHaveLength(0);
    expect(result.skipped).toBe(1);
  });

  it('skips an unparseable timestamp', () => {
    const badTime = JSON.stringify({
      device_id: 42,
      timestamp: 'yesterday',
      temperature: 21.5,
      battery: 90,
      latitude: 0,
      longitude: 0,
      event_type: 'telemetry',
    });
    const result = parseNdjsonBatch(`${badTime}\n`);

    expect(result.valid).toHaveLength(0);
    expect(result.skipped).toBe(1);
  });

  it('skips a device_id too large to represent exactly', () => {
    // The wire type is int64; JS numbers are doubles. Rounding one device's ID
    // into another's is a worse outcome than refusing the line.
    const huge = '{"device_id":9223372036854775807,"timestamp":"2026-08-28T12:34:56Z",' +
      '"temperature":21.5,"battery":90,"latitude":0,"longitude":0,"event_type":"t"}';
    const result = parseNdjsonBatch(`${huge}\n`);

    expect(result.valid).toHaveLength(0);
    expect(result.skipped).toBe(1);
  });

  it('skips a non-integer battery', () => {
    const fractional = JSON.stringify({
      device_id: 42,
      timestamp: '2026-08-28T12:34:56Z',
      temperature: 21.5,
      battery: 90.5,
      latitude: 0,
      longitude: 0,
      event_type: 'telemetry',
    });
    const result = parseNdjsonBatch(`${fractional}\n`);

    expect(result.valid).toHaveLength(0);
    expect(result.skipped).toBe(1);
  });

  it('reports an entirely empty body as empty rather than malformed', () => {
    expect(parseNdjsonBatch('')).toEqual({ valid: [], skipped: 0 });
  });

  it('handles CRLF line endings', () => {
    const result = parseNdjsonBatch(`${validLine}\r\n`);

    expect(result.valid).toHaveLength(1);
    expect(result.skipped).toBe(0);
  });
});
