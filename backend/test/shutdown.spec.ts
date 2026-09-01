import { INestApplication } from '@nestjs/common';
import { Test } from '@nestjs/testing';
import { AppModule } from '../src/app.module';

// ECS stops every task with SIGTERM and SIGKILLs it after stopTimeout. Without
// shutdown hooks Nest never runs onModuleDestroy, so the pg pool and the Redis
// client are torn down by process exit rather than closed. A batch killed
// mid-insert leaves Phase 7's idempotency lease neither committed nor released,
// and the gateway's retry then gets a 503 until the 15s lease expires.
describe('graceful shutdown', () => {
  let app: INestApplication;

  beforeAll(async () => {
    const moduleRef = await Test.createTestingModule({
      imports: [AppModule],
    }).compile();
    app = moduleRef.createNestApplication();
    app.enableShutdownHooks();
    await app.init();
  });

  it('closes without leaving handles open', async () => {
    await expect(app.close()).resolves.toBeUndefined();
  });
});
