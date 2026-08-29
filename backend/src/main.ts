import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import * as express from 'express';
import { AppModule } from './app.module';
import { loadConfig } from './config';

async function bootstrap() {
  const config = loadConfig();
  const app = await NestFactory.create(AppModule);

  // The sink sends application/x-ndjson, which no default parser handles. Take
  // it as raw text and let the parser in Task 3 split it; a JSON parser would
  // reject the whole body, since NDJSON is not a single JSON document.
  app.use(express.text({ type: 'application/x-ndjson', limit: '32mb' }));

  await app.listen(config.port);
}

void bootstrap();
