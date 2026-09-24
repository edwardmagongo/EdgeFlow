# EdgeFlow dashboard

React + Vite dashboard for the EdgeFlow ingestion backend: a live health strip
and a paginated telemetry explorer.

```bash
npm install
npm run dev     # http://localhost:5173, talks to the backend at http://localhost:3000
npm test        # vitest
npm run lint    # oxlint
npm run build   # type-check and bundle into dist/
```

Set `VITE_API_BASE_URL` to point it at a different backend. See the
[Dashboard](../README.md#dashboard) section of the main README for details.
