#!/usr/bin/env node
'use strict';
// Attribution harness for Phase 9. Measures the write path in isolation --
// no gateway, no simulator, no HTTP -- so a run takes seconds and many
// repetitions are cheap. The end-to-end number stays Phase 6's method; this
// instrument exists to say WHERE the per-request time goes.
//
// CORRECTION (fix round, post-review): the original `nosync` variant was
// compared directly against `current` to judge fsync's cost, but the two
// arms did not do the same number of round trips (`nosync` adds a `SET`
// and a restore that `current` never pays), so the comparison was
// confounded by protocol overhead of about the same size as the effect
// being measured. Fixed by adding a `sync` control variant that is
// byte-for-byte identical to `nosync` except for the flag value, and
// comparing `nosync` against `sync` (not against `current`) to isolate
// fsync's cost.
//
// CORRECTION (second fix round, post-review): DELETE-based cleanup restored
// row counts but not physical index size -- a mass insert+delete cycle
// leaves btree indexes bloated indefinitely (verified: ~14x heap size for
// the same live row count after this harness's historical use), and index
// maintenance is part of what `insert` measures. Fixed by REINDEXing the
// two `events` indexes after cleanup on every run, and by printing the
// table's row count and index size every run so drift is visible without a
// separate query.
const { Pool } = require('pg');
const { EventsRepository } = require('../dist/db/events.repository');

const ROWS_PER_BATCH = 100;
const DEFAULT_REPS = 2000;
const WARMUP_REPS = 20;
// Synthetic device IDs used by this harness. Chosen to sit well above any
// ID used by the app or by the test suite (verified empirically: real/test
// data in this database tops out at device_id 499) so the cleanup below can
// delete exactly what this harness inserted without touching anything else.
const BENCH_DEVICE_ID_FLOOR = 9000;

function makeBatch(deviceId, count) {
  const rows = [];
  const base = Date.now();
  for (let i = 0; i < count; i += 1) {
    rows.push({
      deviceId,
      timestamp: new Date(base + i * 1000),
      temperature: 20 + (i % 10),
      battery: 100 - (i % 50),
      latitude: 37.7749,
      longitude: -122.4194,
      eventType: 'telemetry',
    });
  }
  return rows;
}

function buildInsert(rows) {
  const values = [];
  const tuples = [];
  rows.forEach((row, index) => {
    const b = index * 7;
    tuples.push(`($${b + 1}, $${b + 2}, $${b + 3}, $${b + 4}, $${b + 5}, $${b + 6}, $${b + 7})`);
    values.push(row.deviceId, row.timestamp, row.temperature, row.battery,
      row.latitude, row.longitude, row.eventType);
  });
  const text =
    'INSERT INTO events (device_id, timestamp, temperature, battery, latitude, longitude, event_type) VALUES ' +
    tuples.join(', ');
  return { text, values };
}

function median(xs) {
  const s = [...xs].sort((a, b) => a - b);
  const m = Math.floor(s.length / 2);
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}

// Linear-interpolation quantile (same convention as numpy's default) so p90
// gives a meaningful reading even at modest sample sizes.
function quantile(xs, q) {
  const s = [...xs].sort((a, b) => a - b);
  const pos = (s.length - 1) * q;
  const lo = Math.floor(pos);
  const hi = Math.ceil(pos);
  if (lo === hi) return s[lo];
  return s[lo] + (pos - lo) * (s[hi] - s[lo]);
}

// A single median hides bimodal distributions (e.g. half the reps hitting a
// warm cache, half not). Report p90 and the range alongside it so a stable
// median can be told apart from a noisy one at a glance (IMPORTANT 4).
function stats(xs) {
  // MINOR fix (second fix round): `Math.min(...xs)`/`Math.max(...xs)` spread
  // the whole sample array onto the call stack, which throws `RangeError:
  // Maximum call stack size exceeded` above roughly 65k reps. A plain
  // reduce has no such ceiling.
  let min = Infinity;
  let max = -Infinity;
  for (const x of xs) {
    if (x < min) min = x;
    if (x > max) max = x;
  }
  return {
    median: median(xs),
    p90: quantile(xs, 0.9),
    min,
    max,
  };
}

function fmtStats(s) {
  return `median ${s.median.toFixed(3)} ms  p90 ${s.p90.toFixed(3)} ms  min ${s.min.toFixed(3)} ms  max ${s.max.toFixed(3)} ms`;
}

function ms(nanos) { return Number(nanos) / 1e6; }

// --- variants -------------------------------------------------------------
// Each performs ONE batch insert and resolves. Signature is uniform so the
// interleaved driver can treat them identically.

async function current(ctx, rows) {
  await ctx.repo.insertEvents(rows);
}

async function notxn(ctx, rows) {
  const { text, values } = buildInsert(rows);
  await ctx.pool.query(text, values);
}

async function prepared(ctx, rows) {
  const { text, values } = buildInsert(rows);
  // A NAMED query makes pg use the extended protocol and reuse the parsed
  // plan per connection. The name encodes row count because the statement
  // text varies with it.
  await ctx.pool.query({ name: `bench_ins_${rows.length}`, text, values });
}

// `sync` and `nosync` are a MATCHED PAIR (CRITICAL 1 fix): both do the exact
// same sequence of round trips -- connect, SET, BEGIN, INSERT, COMMIT,
// RESET, release -- and differ ONLY in the value of synchronous_commit. Any
// timing delta between them is therefore attributable to the flag itself,
// not to a different number of round trips. Neither should be compared
// directly against `current`/`notxn`/`prepared`, which do not pay the
// SET/RESET round trips at all -- that comparison is exactly the confound
// this fix removes.
async function withSynchronousCommit(ctx, rows, value) {
  const client = await ctx.pool.connect();
  let failed = false;
  try {
    await client.query(`SET synchronous_commit = '${value}'`);
    const { text, values } = buildInsert(rows);
    await client.query('BEGIN');
    await client.query(text, values);
    await client.query('COMMIT');
  } catch (error) {
    failed = true;
    throw error;
  } finally {
    try {
      // RESET (not `SET ... 'on'`): this is a pooled connection, and RESET
      // restores the server default regardless of what it was, which is the
      // only value the pool's next borrower should ever see (MINOR fix).
      await client.query('RESET synchronous_commit');
    } catch (error) {
      // If the restore itself fails, the connection may still be carrying
      // relaxed durability. Do not let it go back to the pool (MINOR fix):
      // a swallowed restore failure previously returned the connection
      // unconditionally, silently corrupting later measurements on whatever
      // variant next drew that connection.
      failed = true;
    }
    client.release(failed);
  }
}

async function sync(ctx, rows) {
  // CONTROL for nosync. Diagnostic only, session-level, never persisted to
  // server config.
  await withSynchronousCommit(ctx, rows, 'on');
}

async function nosync(ctx, rows) {
  // DIAGNOSTIC ONLY. Session-level, never persisted to server config: this
  // exists to size commit-fsync's share of the critical path, and answers
  // whether statement-level work can pay at all. Compare against `sync`
  // (the matched control above), not against `current`.
  await withSynchronousCommit(ctx, rows, 'off');
}

const VARIANTS = { current, notxn, prepared, sync, nosync };

// --- per-step breakdown ---------------------------------------------------

async function stepBreakdown(ctx, rows) {
  const client = await ctx.pool.connect();
  const t = {};
  try {
    let mark = process.hrtime.bigint();
    await client.query('BEGIN');
    t.begin = ms(process.hrtime.bigint() - mark);

    mark = process.hrtime.bigint();
    const { text, values } = buildInsert(rows);
    t.build = ms(process.hrtime.bigint() - mark);

    mark = process.hrtime.bigint();
    await client.query(text, values);
    t.insert = ms(process.hrtime.bigint() - mark);

    mark = process.hrtime.bigint();
    await client.query('COMMIT');
    t.commit = ms(process.hrtime.bigint() - mark);
  } finally {
    client.release();
  }
  return t;
}

// --- drivers --------------------------------------------------------------

async function runVariants(ctx, names, reps, deviceIdBase) {
  const timings = {};
  names.forEach((n) => { timings[n] = []; });
  // INTERLEAVED: one rep of every variant, then repeat. Blocked runs would
  // attribute this machine's background drift to the variant.
  for (let rep = 0; rep < reps; rep += 1) {
    for (const name of names) {
      const rows = makeBatch(deviceIdBase + rep, ROWS_PER_BATCH);
      const start = process.hrtime.bigint();
      await VARIANTS[name](ctx, rows);
      timings[name].push(ms(process.hrtime.bigint() - start));
    }
  }
  return timings;
}

// IMPORTANT 5 fix: this harness previously left every row it inserted in the
// shared `events` table -- roughly 300k rows accumulated across runs, and
// every cross-task comparison drifted as the table (and its composite
// index) grew. A dedicated scratch table was considered and rejected: the
// `current` variant must exercise the real, compiled `insertEvents`, which
// has the real table name baked into its SQL, so routing it at a different
// table would mean measuring code the app doesn't run. Instead, the harness
// deletes exactly the synthetic rows it inserted, identified by the
// disjoint device_id range above, after every run. This keeps every variant
// -- including `current` -- writing to the real table with its real
// indexes (so the measurement stays representative), and restores the row
// count. It does NOT by itself restore physical index size -- see
// `reindexBenchIndexes` below (IMPORTANT 2 fix, second fix round) for that.
// `events` holds only synthetic benchmark/test data and the test suite
// already truncates it routinely, so this is safe.
async function cleanupBenchRows(ctx) {
  const result = await ctx.pool.query('DELETE FROM events WHERE device_id >= $1', [BENCH_DEVICE_ID_FLOOR]);
  return result.rowCount;
}

// IMPORTANT 2 fix (second fix round): the comment above claimed cleanup
// "leaves no residue" -- true for row count, false for physical size.
// DELETE lets autovacuum reclaim heap space for reuse, but it does not
// shrink btree indexes or re-densify their sparse internal pages, and index
// maintenance is a component of `insert` -- the very metric every gate in
// this report, and Task 6's before/after, depends on. Verified before this
// fix: `events`'s two indexes measured ~187 MB against a 13 MB heap for the
// same 120,786 live rows (~14x overhead), residue of the ~1.3M rows earlier
// harness runs inserted and deleted. Left unaddressed, this footprint grows
// monotonically with every future run.
//
// A dedicated scratch table was considered and rejected again here, for the
// same reason IMPORTANT 5 rejected it: `current` must exercise the real,
// compiled `insertEvents`, which has the table name `events` baked into its
// SQL. Instead, both indexes are REINDEXed after cleanup, every run, so the
// table returns to a dense baseline regardless of how much churn the run
// generated -- a deterministic, reproducible physical state per invocation.
//
// Tradeoff: `REINDEX ... CONCURRENTLY` (chosen over plain `REINDEX INDEX`,
// which takes an ACCESS EXCLUSIVE lock for the duration of the rebuild) does
// not block concurrent reads/writes on `events` -- important because this is
// the same shared table the app and test suite use -- but it takes
// noticeably longer than DELETE alone (a full index rebuild by scanning the
// heap) and cannot run inside a transaction block. It rebuilds only the two
// named index relations; it does not touch heap/table bloat (already
// reclaimed by autovacuum) and it never deletes or modifies rows, so the
// live acceptance-run rows below BENCH_DEVICE_ID_FLOOR are untouched.
//
// Failure handling (final whole-branch review): a failed REINDEX ... CONCURRENTLY
// does NOT reproduce the silent-skip hazard migration 002 documents -- the
// rebuild happens under a suffixed `_ccnew` name while the original keeps its
// name and stays valid, so the table is never left without a usable index. But
// it does leave an INVALID `_ccnew` sibling behind, which consumes disk and adds
// write-maintenance overhead to every later INSERT into `events` -- from this
// harness, the app, or the test suite, all of which share this table -- until a
// human drops it. Unlike the one-shot migration, this runs on EVERY invocation,
// so the failure is worth naming loudly with its remediation rather than letting
// a bare stack trace imply the table is untouched. The error is rethrown: a run
// whose physical baseline was not restored must not go on to report timings.
async function reindexBenchIndexes(ctx) {
  for (const index of ['events_pkey', 'events_device_id_timestamp_id_idx']) {
    try {
      await ctx.pool.query(`REINDEX INDEX CONCURRENTLY ${index}`);
    } catch (error) {
      console.error(
        `\nREINDEX of ${index} failed: ${error.message}\n` +
          `An INVALID "${index}_ccnew" index may have been left behind. Check with:\n` +
          `  SELECT c.relname, i.indisvalid FROM pg_class c JOIN pg_index i\n` +
          `    ON i.indexrelid = c.oid WHERE c.relname LIKE '${index}%';\n` +
          `and if one is present and invalid, reclaim it with:\n` +
          `  DROP INDEX CONCURRENTLY ${index}_ccnew;\n` +
          `Timings from this run are not comparable to others until that is done.\n`,
      );
      throw error;
    }
  }
}

// Prints live row count plus heap/index/total size so physical drift is
// visible in the run's own output instead of requiring a separate query
// (IMPORTANT 2 fix, second fix round).
async function reportTableFootprint(ctx, label) {
  const { rows } = await ctx.pool.query(`
    SELECT
      (SELECT count(*) FROM events) AS live_rows,
      pg_relation_size('events') AS heap_bytes,
      pg_indexes_size('events') AS indexes_bytes,
      pg_total_relation_size('events') AS total_bytes
  `);
  const row = rows[0];
  const mb = (bytes) => (Number(bytes) / (1024 * 1024)).toFixed(1);
  console.log(
    `${label}: live_rows ${row.live_rows}  heap ${mb(row.heap_bytes)} MB  ` +
    `indexes ${mb(row.indexes_bytes)} MB  total ${mb(row.total_bytes)} MB`
  );
}

async function main() {
  const args = process.argv.slice(2);
  const modeArg = (args.find((a) => a.startsWith('--mode=')) || '--mode=variants').split('=')[1];
  const repsArg = (args.find((a) => a.startsWith('--reps=')) || `--reps=${DEFAULT_REPS}`).split('=')[1];

  // MINOR fix: validate both. `--mode=step` (singular, a plausible typo)
  // previously fell through silently to the variants branch, and a
  // non-numeric --reps produced NaN that surfaced only much later as a
  // confusing TypeError deep in the loop.
  if (modeArg !== 'steps' && modeArg !== 'variants') {
    console.error(`Invalid --mode=${modeArg}; expected "steps" or "variants"`);
    process.exit(1);
  }
  const reps = Number(repsArg);
  if (!Number.isInteger(reps) || reps <= 0) {
    console.error(`Invalid --reps=${repsArg}; expected a positive integer`);
    process.exit(1);
  }
  const mode = modeArg;

  if (!process.env.DATABASE_URL) {
    console.error('DATABASE_URL is required');
    process.exit(1);
  }
  const pool = new Pool({ connectionString: process.env.DATABASE_URL });
  const ctx = { pool, repo: new EventsRepository(pool) };

  console.log(`load average at start: ${require('os').loadavg().map((n) => n.toFixed(2)).join(' ')}`);
  console.log(`rows per batch: ${ROWS_PER_BATCH}, reps: ${reps}`);
  await reportTableFootprint(ctx, 'table footprint before run');

  try {
    // Warm every variant (MINOR fix), not just `current`. `prepared` in
    // particular has a one-time Parse per physical connection for its named
    // statement; warming only `current` left every other variant, including
    // `prepared`, to pay a cold first call inside the timed run. Note this
    // is a best-effort warmup, not a guarantee: pg.Pool holds up to 10
    // connections by default, and a named statement's parsed plan is cached
    // per physical connection, so WARMUP_REPS interleaved reps may not
    // reach every connection the timed run later draws. Any residual cold
    // start is at least spread across the run instead of concentrated
    // entirely in one variant.
    // NOTE: warmup device IDs must also be >= BENCH_DEVICE_ID_FLOOR, or the
    // cleanup below will not catch them and every invocation leaks
    // WARMUP_REPS * variant-count * ROWS_PER_BATCH rows -- precisely the
    // pollution this fix exists to prevent, just relocated to warmup.
    await runVariants(ctx, Object.keys(VARIANTS), WARMUP_REPS, BENCH_DEVICE_ID_FLOOR);
    await cleanupBenchRows(ctx); // warmup rows are also synthetic; do not leave them either

    if (mode === 'steps') {
      const samples = [];
      for (let i = 0; i < reps; i += 1) {
        samples.push(await stepBreakdown(ctx, makeBatch(BENCH_DEVICE_ID_FLOOR + 500 + i, ROWS_PER_BATCH)));
      }
      for (const key of ['begin', 'build', 'insert', 'commit']) {
        console.log(`  ${key.padEnd(7)} ${fmtStats(stats(samples.map((s) => s[key])))}`);
      }
      const totals = samples.map((s) => s.begin + s.build + s.insert + s.commit);
      console.log(`  ${'TOTAL'.padEnd(7)} ${fmtStats(stats(totals))}`);
    } else {
      const names = Object.keys(VARIANTS);
      const timings = await runVariants(ctx, names, reps, BENCH_DEVICE_ID_FLOOR);
      for (const name of names) {
        const s = stats(timings[name]);
        console.log(`  ${name.padEnd(9)} ${fmtStats(s)}  =>  ${Math.round((ROWS_PER_BATCH / s.median) * 1000).toLocaleString()} events/sec (median)`);
      }
    }

    const deleted = await cleanupBenchRows(ctx);
    console.log(`cleanup: deleted ${deleted} synthetic bench rows (device_id >= ${BENCH_DEVICE_ID_FLOOR})`);
    await reindexBenchIndexes(ctx);
    await reportTableFootprint(ctx, 'table footprint after cleanup+reindex');
  } finally {
    await pool.end();
  }
  console.log(`load average at end:   ${require('os').loadavg().map((n) => n.toFixed(2)).join(' ')}`);
}

main().catch((error) => { console.error(error); process.exit(1); });
