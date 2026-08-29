// The opaque pagination token: a client only ever passes back a value it
// received from a previous response's next_cursor, never one it constructs
// itself. Base64-encoded JSON is enough obfuscation for that contract without
// needing real cryptographic opacity.
export interface Cursor {
  timestamp: string;
  id: number;
}

export function encodeCursor(cursor: Cursor): string {
  return Buffer.from(JSON.stringify(cursor), 'utf8').toString('base64');
}

// Returns null for any malformed input rather than throwing, so the caller
// can treat "invalid cursor" as one uniform 400 case regardless of exactly
// how it was invalid.
export function decodeCursor(value: string): Cursor | null {
  let parsed: unknown;
  try {
    const decoded = Buffer.from(value, 'base64').toString('utf8');
    parsed = JSON.parse(decoded);
  } catch {
    return null;
  }

  if (typeof parsed !== 'object' || parsed === null) {
    return null;
  }
  const candidate = parsed as { timestamp?: unknown; id?: unknown };
  if (typeof candidate.timestamp !== 'string' || typeof candidate.id !== 'number') {
    return null;
  }
  if (!Number.isSafeInteger(candidate.id)) {
    return null;
  }
  if (Number.isNaN(new Date(candidate.timestamp).getTime())) {
    return null;
  }

  return { timestamp: candidate.timestamp, id: candidate.id };
}
