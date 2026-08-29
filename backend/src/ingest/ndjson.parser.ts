// One validated telemetry event, ready to insert.
export interface EventRow {
  deviceId: number;
  timestamp: Date;
  temperature: number;
  battery: number;
  latitude: number;
  longitude: number;
  eventType: string;
}

export interface ParsedBatch {
  valid: EventRow[];
  skipped: number;
}

function isFiniteNumber(value: unknown): value is number {
  // Number.isFinite is deliberate: JSON.parse turns an overflowing literal such
  // as 1e400 into Infinity rather than throwing, and an Infinity stored and
  // re-serialised comes back as null.
  return typeof value === 'number' && Number.isFinite(value);
}

// Returns null when the line is not a usable event. The caller counts it.
function toEventRow(line: string): EventRow | null {
  let parsed: unknown;
  try {
    parsed = JSON.parse(line);
  } catch {
    return null;
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    return null;
  }
  const record = parsed as Record<string, unknown>;

  const {
    device_id: deviceId,
    timestamp,
    temperature,
    battery,
    latitude,
    longitude,
    event_type: eventType,
  } = record;

  // int64 on the wire, double in JavaScript. Beyond 2^53 the value would be
  // rounded, silently attributing an event to a different device.
  if (typeof deviceId !== 'number' || !Number.isSafeInteger(deviceId)) return null;
  if (typeof battery !== 'number' || !Number.isSafeInteger(battery)) return null;
  if (!isFiniteNumber(temperature)) return null;
  if (!isFiniteNumber(latitude)) return null;
  if (!isFiniteNumber(longitude)) return null;
  if (typeof eventType !== 'string') return null;
  if (typeof timestamp !== 'string' || timestamp.length === 0) return null;

  const parsedTimestamp = new Date(timestamp);
  if (Number.isNaN(parsedTimestamp.getTime())) return null;

  return {
    deviceId,
    timestamp: parsedTimestamp,
    temperature,
    battery,
    latitude,
    longitude,
    eventType,
  };
}

// Splits an NDJSON body and validates each line independently.
//
// Per line, never per batch: one bad line must not cost the good ones. Under
// the sink's contract a 4xx response permanently drops the whole batch with no
// retry, so failing the batch here would destroy up to 99 valid events to
// reject one invalid neighbour.
export function parseNdjsonBatch(body: string): ParsedBatch {
  const valid: EventRow[] = [];
  let skipped = 0;

  for (const rawLine of body.split('\n')) {
    // Blank lines are not malformed: FileSink terminates every line, including
    // the last, so a trailing newline is ordinary output.
    const line = rawLine.trim();
    if (line.length === 0) continue;

    const row = toEventRow(line);
    if (row === null) {
      skipped += 1;
    } else {
      valid.push(row);
    }
  }

  return { valid, skipped };
}
