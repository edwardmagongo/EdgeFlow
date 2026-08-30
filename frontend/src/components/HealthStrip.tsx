import { useEffect, useState } from 'react';
import { getHealth, type HealthResponse } from '../api';

const POLL_INTERVAL_MS = 5000;

type Status = 'checking' | 'healthy' | 'degraded' | 'unreachable';

function deriveStatus(health: HealthResponse): { status: Status; detail?: string } {
  if (health.dependencies.database && health.dependencies.redis) {
    return { status: 'healthy' };
  }
  const down = !health.dependencies.database ? 'database' : 'redis';
  return { status: 'degraded', detail: `${down} unreachable` };
}

export function HealthStrip() {
  const [status, setStatus] = useState<Status>('checking');
  const [detail, setDetail] = useState<string | undefined>(undefined);
  const [health, setHealth] = useState<HealthResponse | null>(null);

  useEffect(() => {
    let cancelled = false;

    async function poll() {
      try {
        const result = await getHealth();
        if (cancelled) return;
        const derived = deriveStatus(result);
        setStatus(derived.status);
        setDetail(derived.detail);
        setHealth(result);
      } catch {
        if (cancelled) return;
        setStatus('unreachable');
        setDetail(undefined);
        setHealth(null);
      }
    }

    void poll();
    const intervalId = setInterval(() => void poll(), POLL_INTERVAL_MS);

    return () => {
      cancelled = true;
      clearInterval(intervalId);
    };
  }, []);

  return (
    <div className="health-strip" data-testid="health-strip">
      {health && (
        <>
          <div data-testid="counter-batches_received">
            <span className="label">received</span>
            <strong>{health.counters.batches_received}</strong>
          </div>
          <div data-testid="counter-events_stored">
            <span className="label">stored</span>
            <strong>{health.counters.events_stored}</strong>
          </div>
          <div data-testid="counter-events_skipped_malformed">
            <span className="label">skipped</span>
            <strong>{health.counters.events_skipped_malformed}</strong>
          </div>
          <div data-testid="counter-batches_duplicate_suppressed">
            <span className="label">dup suppressed</span>
            <strong>{health.counters.batches_duplicate_suppressed}</strong>
          </div>
          <div data-testid="counter-db_failures">
            <span className="label">db_failures</span>
            <strong>{health.counters.db_failures}</strong>
          </div>
          <div data-testid="counter-redis_unavailable">
            <span className="label">redis_unavailable</span>
            <strong>{health.counters.redis_unavailable}</strong>
          </div>
        </>
      )}
      <div className={`status-pill status-${status}`} data-testid="status-pill">
        {status === 'checking' && 'checking...'}
        {status === 'healthy' && '● healthy'}
        {status === 'degraded' && `● degraded — ${detail}`}
        {status === 'unreachable' && '● unreachable'}
      </div>
    </div>
  );
}
