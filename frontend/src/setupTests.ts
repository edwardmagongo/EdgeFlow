import '@testing-library/jest-dom/vitest';
import { afterEach } from 'vitest';
import { cleanup } from '@testing-library/react';

// @testing-library/react only auto-registers its afterEach(cleanup) when it
// detects a *global* `afterEach` function. This project's vitest config does
// not set `test.globals: true`, so that detection never fires and rendered
// trees (and their mounted effects/intervals) leak across tests in the same
// file. Register cleanup explicitly so every test starts from an empty DOM.
afterEach(() => {
  cleanup();
});
