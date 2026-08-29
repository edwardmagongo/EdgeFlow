-- Phase 7 query index. IF NOT EXISTS because backend/scripts/migrate.js has
-- no tracking table and re-applies every migration file on every run.
--
-- One composite index serves the keyset-pagination query in both sort
-- directions (order=desc and order=asc): a btree index scans efficiently
-- backward as well as forward, so a single (device_id, timestamp, id) index
-- covers "WHERE device_id = ? ORDER BY timestamp DESC, id DESC" and its
-- ascending counterpart without needing a second, differently-ordered index.
CREATE INDEX IF NOT EXISTS events_device_id_timestamp_id_idx
    ON events (device_id, timestamp, id);
