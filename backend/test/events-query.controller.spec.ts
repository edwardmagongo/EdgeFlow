import { INestApplication } from '@nestjs/common';
import { Test } from '@nestjs/testing';
import request from 'supertest';
import { EventsQueryController } from '../src/events-query/events-query.controller';
import { EventsQueryService } from '../src/events-query/events-query.service';

// Pure unit test of the controller's catch/re-throw logic, isolated from the
// real EventsQueryService and Postgres entirely: proves the 503 path is
// reached only for genuine non-HTTP errors (e.g. a downstream connection
// failure), not for validation errors, which must surface as their own
// HttpException (see events-query.spec.ts for the end-to-end 400 cases).
describe('EventsQueryController (unit)', () => {
  let app: INestApplication;

  beforeAll(async () => {
    const moduleRef = await Test.createTestingModule({
      controllers: [EventsQueryController],
      providers: [
        {
          provide: EventsQueryService,
          useValue: { query: jest.fn().mockRejectedValue(new Error('connection lost')) },
        },
      ],
    }).compile();
    app = moduleRef.createNestApplication();
    await app.init();
  });

  afterAll(async () => {
    await app.close();
  });

  it('maps a non-HTTP error from the service to a 503', async () => {
    const res = await request(app.getHttpServer()).get('/v1/events?device_id=1');
    expect(res.status).toBe(503);
  });
});
