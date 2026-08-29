import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

describe('toolchain sanity check', () => {
  it('renders a component and finds it via Testing Library', () => {
    render(<div>toolchain ready</div>);
    expect(screen.getByText('toolchain ready')).toBeInTheDocument();
  });
});
