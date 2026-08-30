import { act, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { HealthStrip } from './HealthStrip';
import * as api from '../api';

vi.mock('../api');

// `getHealth()` resolves/rejects on a microtask, and the resulting setState
// is only guaranteed to be committed to the DOM once React's update has been
// flushed inside `act()`. A bare `await vi.advanceTimersByTimeAsync(ms)` lets
// the promise settle but does not reliably flush that commit, which makes
// assertions immediately afterward race the render. Wrapping the advance in
// `act()` makes the resulting DOM state deterministic.
async function tick(ms: number) {
  await act(async () => {
    await vi.advanceTimersByTimeAsync(ms);
  });
}

const healthyResponse = {
  status: 'ok',
  dependencies: { database: true, redis: true },
  counters: {
    batches_received: 5,
    batches_duplicate_suppressed: 0,
    batches_in_flight_rejected: 2,
    events_stored: 5,
    events_skipped_malformed: 0,
    db_failures: 0,
    redis_unavailable: 0,
  },
};

describe('HealthStrip', () => {
  afterEach(() => {
    vi.useRealTimers();
    // `vi.restoreAllMocks()` only restores spies created with `vi.spyOn` back
    // to their original implementation; it does not clear call history or
    // queued implementations on a module auto-mocked via `vi.mock('../api')`.
    // Use `resetAllMocks` so each test starts with a clean call count and no
    // leftover mockResolvedValue/mockRejectedValue from a previous test.
    vi.resetAllMocks();
  });

  it('shows healthy status and counters when both dependencies are up', async () => {
    vi.mocked(api.getHealth).mockResolvedValue(healthyResponse);

    render(<HealthStrip />);

    await waitFor(() => {
      expect(screen.getByTestId('status-pill')).toHaveTextContent('healthy');
    });
    expect(screen.getByTestId('counter-events_stored')).toHaveTextContent('5');
    expect(screen.getByTestId('counter-batches_in_flight_rejected')).toHaveTextContent('2');
  });

  it('shows degraded status naming the unreachable dependency', async () => {
    vi.mocked(api.getHealth).mockResolvedValue({
      ...healthyResponse,
      dependencies: { database: false, redis: true },
    });

    render(<HealthStrip />);

    await waitFor(() => {
      expect(screen.getByTestId('status-pill')).toHaveTextContent('degraded');
    });
    expect(screen.getByTestId('status-pill')).toHaveTextContent('database unreachable');
  });

  it('shows unreachable and hides counters when the fetch itself fails', async () => {
    vi.mocked(api.getHealth).mockRejectedValue(new Error('network error'));

    render(<HealthStrip />);

    await waitFor(() => {
      expect(screen.getByTestId('status-pill')).toHaveTextContent('unreachable');
    });
    expect(screen.queryByTestId('counter-events_stored')).not.toBeInTheDocument();
  });

  it('recovers to healthy once the backend responds again after being unreachable', async () => {
    vi.mocked(api.getHealth).mockRejectedValueOnce(new Error('network error'));
    vi.mocked(api.getHealth).mockResolvedValueOnce(healthyResponse);
    vi.useFakeTimers();

    render(<HealthStrip />);
    await tick(0);
    expect(screen.getByTestId('status-pill')).toHaveTextContent('unreachable');

    await tick(5000);
    expect(screen.getByTestId('status-pill')).toHaveTextContent('healthy');
  });

  it('polls again after 5 seconds', async () => {
    const getHealthMock = vi.mocked(api.getHealth).mockResolvedValue(healthyResponse);
    vi.useFakeTimers();

    render(<HealthStrip />);
    await tick(0);
    expect(getHealthMock).toHaveBeenCalledTimes(1);

    await tick(5000);
    expect(getHealthMock).toHaveBeenCalledTimes(2);
  });

  it('stops polling after unmount', async () => {
    const getHealthMock = vi.mocked(api.getHealth).mockResolvedValue(healthyResponse);
    vi.useFakeTimers();

    const { unmount } = render(<HealthStrip />);
    await tick(0);
    unmount();

    await tick(5000);
    expect(getHealthMock).toHaveBeenCalledTimes(1);
  });
});
