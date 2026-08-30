#!/usr/bin/env node
'use strict';
// Attribution harness for Phase 9. Measures the write path in isolation --
// no gateway, no simulator, no HTTP -- so a run takes seconds and many
// repetitions are cheap. The end-to-end number stays Phase 6's method; this
// instrument exists to say WHERE the per-request time goes.
const { Pool } = require('pg');
const { EventsRepository } = require('../dist/db/events.repository');

const ROWS_PER_BATCH = 100;
const DEFAULT_REPS = 200;

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

async function nosync(ctx, rows) {
  // DIAGNOSTIC ONLY. Session-level, never persisted to server config: this
  // exists to size commit-fsync's share of the critical path, and answers
  // whether statement-level work can pay at all.
  const client = await ctx.pool.connect();
  try {
    await client.query("SET synchronous_commit = 'off'");
    const { text, values } = buildInsert(rows);
    await client.query('BEGIN');
    await client.query(text, values);
    await client.query('COMMIT');
  } finally {
    await client.query("SET synchronous_commit = 'on'").catch(() => undefined);
    client.release();
  }
}

const VARIANTS = { current, notxn, prepared, nosync };

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

async function runVariants(ctx, names, reps) {
  const timings = {};
  names.forEach((n) => { timings[n] = []; });
  // INTERLEAVED: one rep of every variant, then repeat. Blocked runs would
  // attribute this machine's background drift to the variant.
  for (let rep = 0; rep < reps; rep += 1) {
    for (const name of names) {
      const rows = makeBatch(9000 + rep, ROWS_PER_BATCH);
      const start = process.hrtime.bigint();
      await VARIANTS[name](ctx, rows);
      timings[name].push(ms(process.hrtime.bigint() - start));
    }
  }
  return timings;
}

async function main() {
  const args = process.argv.slice(2);
  const mode = (args.find((a) => a.startsWith('--mode=')) || '--mode=variants').split('=')[1];
  const reps = Number((args.find((a) => a.startsWith('--reps=')) || `--reps=${DEFAULT_REPS}`).split('=')[1]);

  if (!process.env.DATABASE_URL) {
    console.error('DATABASE_URL is required');
    process.exit(1);
  }
  const pool = new Pool({ connectionString: process.env.DATABASE_URL });
  const ctx = { pool, repo: new EventsRepository(pool) };

  console.log(`load average at start: ${require('os').loadavg().map((n) => n.toFixed(2)).join(' ')}`);
  console.log(`rows per batch: ${ROWS_PER_BATCH}, reps: ${reps}`);

  try {
    // Warm the pool and the plan cache; a cold first call is not representative.
    await runVariants(ctx, ['current'], 5);

    if (mode === 'steps') {
      const samples = [];
      for (let i = 0; i < reps; i += 1) {
        samples.push(await stepBreakdown(ctx, makeBatch(9500 + i, ROWS_PER_BATCH)));
      }
      for (const key of ['begin', 'build', 'insert', 'commit']) {
        console.log(`  ${key.padEnd(7)} median ${median(samples.map((s) => s[key])).toFixed(3)} ms`);
      }
      const total = median(samples.map((s) => s.begin + s.build + s.insert + s.commit));
      console.log(`  ${'TOTAL'.padEnd(7)} median ${total.toFixed(3)} ms`);
    } else {
      const names = Object.keys(VARIANTS);
      const timings = await runVariants(ctx, names, reps);
      for (const name of names) {
        const m = median(timings[name]);
        console.log(`  ${name.padEnd(9)} median ${m.toFixed(3)} ms/batch  =>  ${Math.round((ROWS_PER_BATCH / m) * 1000).toLocaleString()} events/sec`);
      }
    }
  } finally {
    await pool.end();
  }
  console.log(`load average at end:   ${require('os').loadavg().map((n) => n.toFixed(2)).join(' ')}`);
}

main().catch((error) => { console.error(error); process.exit(1); });
