import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { EventsExplorer } from './EventsExplorer';
import { ApiError } from '../api';
import * as api from '../api';

// A bare `vi.mock('../api')` automocks every export, including the
// `ApiError` class: Vitest strips the constructor body from automocked
// classes (while preserving the prototype chain), so `new ApiError(msg,
// status)` would silently produce an instance with `message: ''`,
// `name: 'Error'`, and `status: undefined` even though `instanceof ApiError`
// still reports true. That breaks the "shows the backend error message"
// test below, which needs a real ApiError carrying its message through to
// the rendered alert. Keep the real ApiError class via importActual and
// mock only the functions this component calls.
vi.mock('../api', async () => {
  const actual = await vi.importActual<typeof import('../api')>('../api');
  return {
    ...actual,
    queryEvents: vi.fn(),
  };
});

function fillDeviceId(value: string) {
  fireEvent.change(screen.getByLabelText('device_id'), { target: { value } });
}

describe('EventsExplorer', () => {
  afterEach(() => {
    vi.resetAllMocks();
  });

  it('queries with only the provided parameters, omitting blank optional fields', async () => {
    const queryEventsMock = vi.mocked(api.queryEvents).mockResolvedValue({ events: [], next_cursor: null });
    render(<EventsExplorer />);

    fillDeviceId('42');
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));

    await waitFor(() => expect(queryEventsMock).toHaveBeenCalled());
    expect(queryEventsMock).toHaveBeenCalledWith({
      deviceId: 42,
      from: undefined,
      to: undefined,
      limit: undefined,
      order: 'desc',
      cursor: undefined,
    });
  });

  it('forwards every filter field exactly as entered, including a non-default order', async () => {
    const queryEventsMock = vi.mocked(api.queryEvents).mockResolvedValue({ events: [], next_cursor: null });
    render(<EventsExplorer />);

    fillDeviceId('42');
    fireEvent.change(screen.getByLabelText('from'), { target: { value: '2026-01-01T00:00:00Z' } });
    fireEvent.change(screen.getByLabelText('to'), { target: { value: '2026-02-01T00:00:00Z' } });
    fireEvent.change(screen.getByLabelText('limit'), { target: { value: '25' } });
    fireEvent.change(screen.getByLabelText('order'), { target: { value: 'asc' } });
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));

    await waitFor(() => expect(queryEventsMock).toHaveBeenCalled());
    expect(queryEventsMock).toHaveBeenCalledWith({
      deviceId: 42,
      from: '2026-01-01T00:00:00Z',
      to: '2026-02-01T00:00:00Z',
      limit: 25,
      order: 'asc',
      cursor: undefined,
    });
  });

  it('renders returned rows in the results table', async () => {
    vi.mocked(api.queryEvents).mockResolvedValue({
      events: [
        {
          device_id: 42,
          timestamp: '2026-08-29T10:15:00.000Z',
          temperature: 21.5,
          battery: 90,
          latitude: 37.7749,
          longitude: -122.4194,
          event_type: 'telemetry',
        },
      ],
      next_cursor: null,
    });
    render(<EventsExplorer />);

    fillDeviceId('42');
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));

    expect(await screen.findByText('telemetry')).toBeInTheDocument();
    expect(screen.getByText('21.5')).toBeInTheDocument();
  });

  it('shows "No events found" for an empty result set', async () => {
    vi.mocked(api.queryEvents).mockResolvedValue({ events: [], next_cursor: null });
    render(<EventsExplorer />);

    fillDeviceId('999');
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));

    expect(await screen.findByText('No events found')).toBeInTheDocument();
  });

  it('shows a Next button only while next_cursor is non-null, and walks forward on click', async () => {
    const queryEventsMock = vi.mocked(api.queryEvents);
    queryEventsMock.mockResolvedValueOnce({
      events: [{ device_id: 1, timestamp: '2026-08-29T10:02:00.000Z', temperature: 1, battery: 1, latitude: 1, longitude: 1, event_type: 't' }],
      next_cursor: 'cursor-page-2',
    });
    render(<EventsExplorer />);

    fillDeviceId('1');
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));
    await screen.findByRole('button', { name: 'Next →' });

    queryEventsMock.mockResolvedValueOnce({
      events: [{ device_id: 1, timestamp: '2026-08-29T10:01:00.000Z', temperature: 1, battery: 1, latitude: 1, longitude: 1, event_type: 't' }],
      next_cursor: null,
    });
    fireEvent.click(screen.getByRole('button', { name: 'Next →' }));

    await waitFor(() => {
      expect(queryEventsMock).toHaveBeenLastCalledWith(expect.objectContaining({ cursor: 'cursor-page-2' }));
    });
    await waitFor(() => {
      expect(screen.queryByRole('button', { name: 'Next →' })).not.toBeInTheDocument();
    });
  });

  it('walks backward with Previous, re-querying the prior cursor', async () => {
    const queryEventsMock = vi.mocked(api.queryEvents);
    queryEventsMock.mockResolvedValueOnce({ events: [], next_cursor: 'cursor-page-2' });
    render(<EventsExplorer />);

    fillDeviceId('1');
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));
    await screen.findByRole('button', { name: 'Next →' });

    queryEventsMock.mockResolvedValueOnce({ events: [], next_cursor: null });
    fireEvent.click(screen.getByRole('button', { name: 'Next →' }));
    await screen.findByRole('button', { name: '← Previous' });

    queryEventsMock.mockResolvedValueOnce({ events: [], next_cursor: 'cursor-page-2' });
    fireEvent.click(screen.getByRole('button', { name: '← Previous' }));

    await waitFor(() => {
      expect(queryEventsMock).toHaveBeenLastCalledWith(expect.objectContaining({ cursor: undefined }));
    });
    await waitFor(() => {
      expect(screen.queryByRole('button', { name: '← Previous' })).not.toBeInTheDocument();
    });
  });

  it('clears pagination and starts a fresh query when a form field changes and Query is clicked again', async () => {
    const queryEventsMock = vi.mocked(api.queryEvents);
    queryEventsMock.mockResolvedValueOnce({ events: [], next_cursor: 'cursor-page-2' });
    render(<EventsExplorer />);

    fillDeviceId('1');
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));
    await screen.findByRole('button', { name: 'Next →' });

    queryEventsMock.mockResolvedValueOnce({ events: [], next_cursor: null });
    fireEvent.click(screen.getByRole('button', { name: 'Next →' }));
    await screen.findByRole('button', { name: '← Previous' });

    fillDeviceId('2');
    queryEventsMock.mockResolvedValueOnce({ events: [], next_cursor: null });
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));

    await waitFor(() => {
      expect(queryEventsMock).toHaveBeenLastCalledWith(expect.objectContaining({ deviceId: 2, cursor: undefined }));
    });
    await waitFor(() => {
      expect(screen.queryByRole('button', { name: '← Previous' })).not.toBeInTheDocument();
    });
  });

  it('shows the backend error message without clearing previously-displayed results', async () => {
    const queryEventsMock = vi.mocked(api.queryEvents);
    queryEventsMock.mockResolvedValueOnce({
      events: [{ device_id: 1, timestamp: '2026-08-29T10:00:00.000Z', temperature: 1, battery: 1, latitude: 1, longitude: 1, event_type: 'telemetry' }],
      next_cursor: null,
    });
    render(<EventsExplorer />);

    fillDeviceId('1');
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));
    await screen.findByText('telemetry');

    queryEventsMock.mockRejectedValueOnce(new ApiError('device_id is required', 400));
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('device_id is required');
    expect(screen.getByText('telemetry')).toBeInTheDocument();
  });

  it('disables the Query button while a request is in flight', async () => {
    let resolveFn: (value: { events: never[]; next_cursor: null }) => void = () => {};
    vi.mocked(api.queryEvents).mockReturnValue(
      new Promise((resolve) => {
        resolveFn = resolve;
      }),
    );
    render(<EventsExplorer />);

    fillDeviceId('1');
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));

    expect(screen.getByRole('button', { name: /Querying/ })).toBeDisabled();

    resolveFn({ events: [], next_cursor: null });
    await screen.findByText('No events found');
  });
});
