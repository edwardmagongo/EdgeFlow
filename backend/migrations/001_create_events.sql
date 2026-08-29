-- Phase 6 ingestion schema.
--
-- There is deliberately NO unique constraint on (device_id, timestamp). The
-- gateway formats timestamps to whole-second granularity while devices emit at
-- 10-400 events/sec each, so at 50 events/sec fifty legitimate events share one
-- (device_id, timestamp) pair. Such a constraint would silently discard the
-- overwhelming majority of valid telemetry. Deduplication happens at batch
-- granularity via the Idempotency-Key, and nowhere else.
--
-- No secondary indexes yet either: an index chosen with no query workload to
-- serve would be a guess, and it would slow the ingest path this phase exists
-- to measure. Indexing belongs with the query phase.
CREATE TABLE IF NOT EXISTS events (
    id          bigserial PRIMARY KEY,
    device_id   bigint NOT NULL,
    timestamp   timestamptz NOT NULL,
    temperature double precision NOT NULL,
    battery     integer NOT NULL,
    latitude    double precision NOT NULL,
    longitude   double precision NOT NULL,
    event_type  text NOT NULL,
    received_at timestamptz NOT NULL DEFAULT now()
);
