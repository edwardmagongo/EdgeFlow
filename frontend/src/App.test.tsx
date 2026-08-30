import { render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import App from './App';
import * as api from './api';

vi.mock('./api');

describe('App', () => {
  it('renders the health strip and the events explorer', () => {
    vi.mocked(api.getHealth).mockResolvedValue({
      status: 'ok',
      dependencies: { database: true, redis: true },
      counters: {
        batches_received: 0,
        batches_duplicate_suppressed: 0,
        // The plan's fixture for this task predates the seventh counter and
        // lists only six. HealthResponse.counters requires all seven, so the
        // plan's literal code does not typecheck -- the same staleness commit
        // a1a8dbc fixed for Task 4's fixture, still present in Task 6's.
        batches_in_flight_rejected: 0,
        events_stored: 0,
        events_skipped_malformed: 0,
        db_failures: 0,
        redis_unavailable: 0,
      },
    });

    render(<App />);

    expect(screen.getByTestId('health-strip')).toBeInTheDocument();
    expect(screen.getByLabelText('device_id')).toBeInTheDocument();
  });
});
