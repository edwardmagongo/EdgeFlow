import { useState, type FormEvent } from 'react';
import { ApiError, queryEvents, type EventRecord } from '../api';

export function EventsExplorer() {
  const [deviceId, setDeviceId] = useState('');
  const [from, setFrom] = useState('');
  const [to, setTo] = useState('');
  const [limit, setLimit] = useState('');
  const [order, setOrder] = useState<'asc' | 'desc'>('desc');

  const [cursorStack, setCursorStack] = useState<(string | undefined)[]>([undefined]);
  const [results, setResults] = useState<EventRecord[] | null>(null);
  const [nextCursor, setNextCursor] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function runQuery(cursor: string | undefined) {
    setLoading(true);
    setError(null);
    try {
      const response = await queryEvents({
        deviceId: Number(deviceId),
        from: from || undefined,
        to: to || undefined,
        limit: limit ? Number(limit) : undefined,
        order,
        cursor,
      });
      setResults(response.events);
      setNextCursor(response.next_cursor);
    } catch (err) {
      setError(err instanceof ApiError ? err.message : 'Request failed');
    } finally {
      setLoading(false);
    }
  }

  function handleSubmit(event: FormEvent) {
    event.preventDefault();
    setCursorStack([undefined]);
    void runQuery(undefined);
  }

  function handleNext() {
    if (!nextCursor) return;
    const newStack = [...cursorStack, nextCursor];
    setCursorStack(newStack);
    void runQuery(nextCursor);
  }

  function handlePrevious() {
    if (cursorStack.length <= 1) return;
    const newStack = cursorStack.slice(0, -1);
    setCursorStack(newStack);
    void runQuery(newStack[newStack.length - 1]);
  }

  return (
    <div className="events-explorer">
      <form onSubmit={handleSubmit}>
        <input aria-label="device_id" type="number" value={deviceId} onChange={(e) => setDeviceId(e.target.value)} />
        <input aria-label="from" placeholder="from (YYYY-MM-DDTHH:mm:ssZ)" value={from} onChange={(e) => setFrom(e.target.value)} />
        <input aria-label="to" placeholder="to (YYYY-MM-DDTHH:mm:ssZ)" value={to} onChange={(e) => setTo(e.target.value)} />
        <input aria-label="limit" type="number" placeholder="100" value={limit} onChange={(e) => setLimit(e.target.value)} />
        <select aria-label="order" value={order} onChange={(e) => setOrder(e.target.value as 'asc' | 'desc')}>
          <option value="desc">desc</option>
          <option value="asc">asc</option>
        </select>
        <button type="submit" disabled={loading}>
          {loading ? 'Querying…' : 'Query'}
        </button>
      </form>

      {error && <div role="alert">{error}</div>}

      {results !== null &&
        (results.length === 0 ? (
          <p>No events found</p>
        ) : (
          <table>
            <thead>
              <tr>
                <th>timestamp</th>
                <th>device_id</th>
                <th>temperature</th>
                <th>battery</th>
                <th>latitude</th>
                <th>longitude</th>
                <th>event_type</th>
              </tr>
            </thead>
            <tbody>
              {results.map((event, index) => (
                <tr key={`${event.timestamp}-${index}`}>
                  <td>{event.timestamp}</td>
                  <td>{event.device_id}</td>
                  <td>{event.temperature}</td>
                  <td>{event.battery}</td>
                  <td>{event.latitude}</td>
                  <td>{event.longitude}</td>
                  <td>{event.event_type}</td>
                </tr>
              ))}
            </tbody>
          </table>
        ))}

      <div className="pagination">
        {cursorStack.length > 1 && (
          <button type="button" onClick={handlePrevious} disabled={loading}>
            ← Previous
          </button>
        )}
        {nextCursor && (
          <button type="button" onClick={handleNext} disabled={loading}>
            Next →
          </button>
        )}
      </div>
    </div>
  );
}
