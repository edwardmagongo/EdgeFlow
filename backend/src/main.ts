import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import * as express from 'express';
import { AppModule } from './app.module';
import { loadConfig } from './config';

async function bootstrap() {
  const config = loadConfig();
  const app = await NestFactory.create(AppModule);

  // Open to any origin: the dashboard (Phase 8) is a browser SPA served from
  // a different origin during dev, and neither this endpoint nor
  // GET /v1/events has auth -- matching that existing no-auth,
  // trusted-network posture rather than introducing a new one.
  app.enableCors();

  // The sink sends application/x-ndjson, which no default parser handles. Take
  // it as raw text and let the parser in Task 3 split it; a JSON parser would
  // reject the whole body, since NDJSON is not a single JSON document.
  app.use(express.text({ type: 'application/x-ndjson', limit: '32mb' }));

  // ECS stops tasks with SIGTERM, then SIGKILL after stopTimeout, and every
  // deploy stops a task. Without this Nest never runs onModuleDestroy: the pg
  // pool and Redis client are killed rather than closed, and an in-flight
  // ingest dies mid-insert with its idempotency lease still claimed.
  app.enableShutdownHooks();

  await app.listen(config.port);
}

void bootstrap();
