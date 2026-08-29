import { INestApplication } from '@nestjs/common';
import { Test } from '@nestjs/testing';
import { spawn, spawnSync, ChildProcess } from 'child_process';
import * as express from 'express';
import * as fs from 'fs';
import * as path from 'path';
import { Pool } from 'pg';
import { AppModule } from '../src/app.module';
import { PG_POOL } from '../src/db/pool';

const REPO_ROOT = path.resolve(__dirname, '..', '..');
const GATEWAY = path.join(REPO_ROOT, 'build', 'gateway', 'edgeflow-gateway');
const SIMULATOR = path.join(REPO_ROOT, 'build', 'simulator', 'edgeflow-simulator');

const BACKEND_PORT = 3999;
const GATEWAY_PORT = 19300;

const binariesExist = fs.existsSync(GATEWAY) && fs.existsSync(SIMULATOR);
const describeOrSkip = binariesExist ? describe : describe.skip;

if (!binariesExist) {
  console.warn(
    `skipping gateway end-to-end test: build the C++ binaries first ` +
      `(expected ${GATEWAY} and ${SIMULATOR})`,
  );
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

describeOrSkip('gateway -> backend -> postgres', () => {
  let app: INestApplication;
  let pool: Pool;
  let gateway: ChildProcess | null = null;

  beforeAll(async () => {
    const moduleRef = await Test.createTestingModule({ imports: [AppModule] }).compile();
    app = moduleRef.createNestApplication();
    app.use(express.text({ type: 'application/x-ndjson', limit: '32mb' }));
    await app.init();
    await app.listen(BACKEND_PORT);
    pool = app.get<Pool>(PG_POOL);
  });

  afterAll(async () => {
    if (gateway !== null && gateway.exitCode === null) {
      gateway.kill('SIGTERM');
    }
    await app.close();
  });

  it('lands every accepted event as exactly one row', async () => {
    await pool.query('TRUNCATE events');

    gateway = spawn(GATEWAY, [
      `--port=${GATEWAY_PORT}`,
      '--workers=2',
      '--queue-capacity=1024',
      '--sink=http',
      `--sink-url=http://127.0.0.1:${BACKEND_PORT}/v1/events`,
      '--batch-size=50',
      '--batch-age-ms=200',
    ]);

    let gatewayOutput = '';
    gateway.stdout?.on('data', (chunk) => {
      gatewayOutput += String(chunk);
    });

    await sleep(500); // let the gateway bind its port

    const simulator = spawnSync(SIMULATOR, [
      `--port=${GATEWAY_PORT}`,
      '--devices=20',
      '--rate=10',
      '--duration=3',
    ]);
    expect(simulator.status).toBe(0);

    // SIGTERM makes the gateway drain the batcher and the outbound queue, then
    // print its shutdown counters. Without the drain, in-flight batches would
    // still be queued and the row count would legitimately be short.
    gateway.kill('SIGTERM');
    for (let i = 0; i < 100 && gateway.exitCode === null; i += 1) {
      await sleep(100);
    }

    const accepted = /accepted=(\d+)/.exec(gatewayOutput);
    expect(accepted).not.toBeNull();
    const acceptedCount = Number(accepted![1]);
    expect(acceptedCount).toBeGreaterThan(0);

    const stored = await pool.query('SELECT count(*)::int AS n FROM events');
    // Equality, not "close enough": the whole point is that nothing is lost and
    // nothing is duplicated across the language boundary.
    expect(stored.rows[0].n).toBe(acceptedCount);
  }, 60000);
});
