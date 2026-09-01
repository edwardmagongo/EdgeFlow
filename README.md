# EdgeFlow

A concurrent C++20 telemetry pipeline: simulated devices → TCP gateway →
bounded queue → worker pool → batcher → sink.

Phases 1-6 have landed: the C++ core pipeline with a file-backed sink
(Phase 1), a benchmark suite and simulator chaos scenarios (Phase 2), a
lock-free queue variant benchmarked against the mutex one (Phase 3), a
saturating load generator that located the gateway's throughput knee
(Phase 4), an HTTP sink that POSTs NDJSON batches to a backend endpoint
(Phase 5), and a NestJS ingestion backend that validates those batches and
stores them in PostgreSQL, with Redis holding batch-level idempotency keys
(Phase 6). The HTTP sink still talks to any endpoint you point it at. Phase 7
added a read/query API (`GET /v1/events`) and Phase 8 added a dashboard on top
of it. Phase 11 adds reviewable Terraform and a deploy runbook for AWS
(see "Deployment (AWS)" below). It has been applied to a real account and
verified end-to-end: the dashboard and the API served through one CloudFront
distribution, backed by RDS Postgres and ElastiCache Redis. Whether an
environment is running at any given moment depends on whether `terraform
destroy` has been run since; the evidence is in
`docs/superpowers/progress/2026-09-01-docker-aws-deployment-progress.md`.
Each phase has its own spec under `docs/superpowers/specs/` and a plan under
`docs/superpowers/plans/`; Phases 2-6 also have a task-by-task execution log
under `docs/superpowers/progress/`.

## Build

    brew install boost cmake   # macOS prerequisites
    cmake -S . -B build
    cmake --build build

## Test

    ctest --test-dir build --output-on-failure

Sanitizer builds:

    cmake -S . -B build-asan -DEDGEFLOW_SANITIZE=address
    cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

    cmake -S . -B build-tsan -DEDGEFLOW_SANITIZE=thread
    cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure

## Run

Start the gateway:

    ./build/gateway/edgeflow-gateway --port=9000 --workers=4 \
        --queue-capacity=2000 --backpressure=block \
        --batch-size=100 --batch-age-ms=200 \
        --sink-file=/tmp/edgeflow_events.ndjson

Gateway flags: `--port`, `--workers`, `--queue-capacity`, `--backpressure`
(`block`|`drop-oldest`|`drop-newest`), `--batch-size`, `--batch-age-ms`,
`--sink-file`.

HTTP sink flags: `--sink` (`file`|`http`), `--sink-url`,
`--sink-outbound-capacity` (batches held for the backend, default 256),
`--sink-backpressure` (governs the *outbound* queue, separately from
`--backpressure` which governs the event queue), `--sink-concurrency` (sink
threads draining the outbound queue, default 4, max 1024), `--sink-max-retries`,
`--sink-backoff-ms`, `--sink-timeout-ms`. Plain HTTP only -- no TLS, no auth.

In another terminal, run ~1,000 simulated devices for 15 seconds:

    ./build/simulator/edgeflow-simulator --devices=1000 --rate=1 --duration=15

Simulator flags: `--host`, `--port`, `--devices`, `--rate` (events/sec per
device), `--duration` (seconds), `--chaos-latency-ms` (extra per-event send
delay), `--chaos-packet-loss-percent` (0-100, chance a device skips a send --
models a device failing to produce telemetry, not true network packet loss),
`--chaos-device-spike` + `--chaos-device-spike-at-sec` (add N devices mid-run
at the given second).

Inspect the output:

    wc -l /tmp/edgeflow_events.ndjson
    head -n 3 /tmp/edgeflow_events.ndjson

### Ingestion backend

To store events instead of writing them to a file, start PostgreSQL and Redis,
apply the migration, and run the backend:

    docker compose up -d
    cd backend && npm install
    npm run migrate
    npm run build && npm start

Then point the gateway at it:

    ./build/gateway/edgeflow-gateway --port=9000 --workers=4 \
        --sink=http --sink-url=http://127.0.0.1:3000/v1/events \
        --batch-size=100 --batch-age-ms=200

The service reads four environment variables:

- `DATABASE_URL` (required) -- PostgreSQL connection string.
- `REDIS_URL` (required) -- Redis connection string.
- `PORT` (default `3000`) -- the HTTP port to listen on.
- `IDEMPOTENCY_TTL_SECONDS` (default `900`) -- how long a batch's
  `Idempotency-Key` is remembered.

`docker compose up -d` binds PostgreSQL to host port **5433** and Redis to
**6380**, not their defaults, so it does not collide with other containers on
the same machine. Match them:

    export DATABASE_URL=postgres://edgeflow:edgeflow@localhost:5433/edgeflow
    export REDIS_URL=redis://localhost:6380

`POST /v1/events` takes an NDJSON body and an `Idempotency-Key` header (the
HTTP sink mints one per batch) and replies with the per-batch outcome:

    {"received":5,"stored":5,"skipped":0,"duplicate":false}

`GET /v1/health` reports dependency reachability and seven counters:
`batches_received`, `batches_duplicate_suppressed`,
`batches_in_flight_rejected`, `events_stored`, `events_skipped_malformed`,
`db_failures`, `redis_unavailable`.

Invalid lines are skipped individually rather than failing the batch. If
PostgreSQL is unreachable the endpoint returns **503**, never a 4xx, so the
sink retries rather than discarding the batch on the spot. A failed insert
releases the batch's idempotency key, so that retry is a real retry rather than
one suppressed as an already-stored duplicate.

A key is only marked committed once its insert has committed, so
`"duplicate":true` means the rows are durably stored. A retry that arrives
while the first attempt is still inserting gets a **503** and is counted under
`batches_in_flight_rejected`, rather than being told the batch was already
stored -- storing it twice and dropping it entirely are both worse than asking
the sink to come back.

If Redis is unreachable the idempotency check fails open -- the batch is stored
rather than rejected, on the reasoning that losing telemetry is worse than
storing it twice. A Redis outage concurrent with a sink retry therefore admits
duplicate rows by design, and the only record that it happened is the
`redis_unavailable` counter.

Failing open requires *failing*, not waiting: the client sets
`disableOfflineQueue`, so a command issued during an outage is rejected at once
instead of being buffered and replayed when Redis returns. Without it a claim
does not fail open, it blocks for the length of the outage -- which is worse,
because a blocked ingest request burns the sink's entire retry budget and the
batch is dropped as exhausted. For the same reason the readiness guard tests
`isReady` rather than `isOpen`: node-redis keeps `isOpen` true for the whole
reconnect cycle, so `isOpen` reports a usable connection during an outage.

The connection itself retries indefinitely with an exponential backoff capped at
2 seconds, in one regime whether or not it has ever connected. An outage ends
when Redis returns, not when the process restarts, and a Redis that is not up
yet at boot -- the common case when compose or k8s starts both at once -- is
picked up when it appears. Startup never blocks on it: `connect()` waits a
bounded moment for a first connection and then returns, leaving the retry loop
running, so a missing Redis costs deduplication rather than the service's
boot.

On shutdown (SIGINT/SIGTERM), the gateway prints its observability counters:
accepted, dropped_oldest, dropped_newest, and malformed event counts, plus
queue-wait latency (time from a connection enqueueing an event to a worker
popping it), e.g.

    edgeflow-gateway shut down (accepted=14800, dropped_oldest=0, dropped_newest=0, malformed=0, queue_wait_count=14800, queue_wait_mean_us=17.4695, queue_wait_p50_us=100, queue_wait_p99_us=100)

`queue_wait_mean_us` is exact; the p50/p99 figures come from a fixed
histogram whose lowest bucket boundary is 100us, so a reported `p50=100`
means "at or below 100us", not "exactly 100us".

### Dashboard (Phase 8)

A React + Vite single-page app shows the backend's live health counters
and lets you browse a device's telemetry history:

    cd frontend
    npm install
    npm run dev

Open the URL Vite prints (typically `http://localhost:5173`). By default
it talks to the backend at `http://localhost:3000`; point it elsewhere
with `VITE_API_BASE_URL`:

    VITE_API_BASE_URL=http://localhost:4000 npm run dev

The backend must have CORS enabled to accept requests from the
dashboard's origin -- `app.enableCors()` in `backend/src/main.ts` does
this already, open to any origin, matching the backend's existing
no-auth posture.

The health strip polls `GET /v1/health` every 5 seconds. The explorer
queries `GET /v1/events` and paginates with the opaque cursor it
returns -- "Next"/"Previous" walk forward and backward through pages
already fetched in the current session, not through page numbers.

## Deployment (AWS)

Terraform under `infra/` provisions one environment (`prod`) in `eu-west-1`.
One CloudFront distribution fronts both halves of the product: the default
behavior serves the dashboard's static build from a private S3 bucket, and
`/v1/*` routes to an internet-facing ALB in front of a single ECS Fargate task
running the backend. RDS Postgres and ElastiCache Redis sit in private subnets
reachable only from the ECS security group. HTTPS comes from the
distribution's own `*.cloudfront.net` certificate -- there is no custom domain
and no ACM certificate.

Infrastructure and application code are on separate levers: Terraform for the
former, two scripts for the latter.

**This creates billable resources.** RDS `db.t4g.micro`, ElastiCache
`cache.t4g.micro`, an ALB and a CloudFront distribution run roughly $40-60 a
month if left up. `terraform destroy` removes them.

### Prerequisites

```bash
aws sts get-caller-identity && terraform version && docker version
```

Credentials need permission to create VPC, ECS, ECR, RDS, ElastiCache, ALB,
S3, CloudFront, Secrets Manager, IAM and CloudWatch Logs resources. Terraform
must be >= 1.5.

### From a clean account

```bash
cd infra && terraform init && terraform apply
```

Roughly 12-15 minutes; RDS and CloudFront dominate. The ECS service reports
tasks failing to pull an image until the first backend deploy, which is
expected.

```bash
./scripts/deploy-backend.sh    # build, push, migrate, roll
./scripts/deploy-frontend.sh   # build, sync to S3, invalidate CloudFront
```

`deploy-backend.sh` tags images with the git SHA rather than `:latest` and
refuses to build from a dirty tree, so a running task is always traceable to a
commit and the previous image survives for rollback. Migrations run on every
deploy as a one-off ECS task, and the service rolls only if that task exits 0
-- new code cannot end up running against an old schema.

### What Terraform stops managing after the first apply

The two levers are deliberate, but the boundary has a sharp edge worth knowing
before you edit `infra/modules/ecs/main.tf`.

The task definition carries `lifecycle { ignore_changes = [container_definitions] }`
so Terraform does not fight `deploy-backend.sh` over the image tag. Terraform
cannot ignore one nested field of a JSON blob, so this ignores the **whole**
container definition. After the first apply, changing any of these in Terraform
has no effect at all, silently:

- `stopTimeout`
- the `PORT` and `IDEMPOTENCY_TTL_SECONDS` environment variables
- the `secrets` ARNs
- the log configuration

To change one, either edit it and force a new revision through
`deploy-backend.sh`, or temporarily remove the `ignore_changes` block, apply, and
put it back.

Separately, the service carries `ignore_changes = [task_definition]`. `cpu` and
`memory` sit outside `container_definitions`, so editing those *does* produce a
new task definition revision -- but the service will not adopt it until the next
`deploy-backend.sh` run.

### Redeploying the same commit

ECR is set to `IMMUTABLE` tags, which is what makes a git SHA identify exactly
one image. The cost is that `deploy-backend.sh` cannot push a SHA twice: if a
deploy fails after the push (a failing migration, say), fixing the cause and
re-running the script aborts on the push. Make an empty commit
(`git commit --allow-empty`) and deploy that.

### Health endpoints

Two, deliberately:

- `GET /v1/health` reports dependency reachability and the ingest counters.
  Its `status` is `"ok"` when both Postgres and Redis are reachable and
  `"degraded"` when either is not. It answers **200 either way**, so it is a
  diagnostic to read, not a check to route on.
- `GET /v1/health/live` reports only that the process is up. This is what the
  ALB target group checks.

A dependency outage bounds rather than blocks `/v1/health`: the pg pool carries
a 3s `connectionTimeoutMillis`, so an unreachable database reports
`"database": false` in about three seconds instead of hanging. Without that
bound a revoked security group rule drops packets, and the query waits forever
rather than failing -- which is what an earlier acceptance run actually hit.

The split is not cosmetic. With one ECS task and one shared RDS instance,
failing the load balancer check on a database outage would deregister the only
target and turn the backend's own retryable 503s into CloudFront 502s, without
routing around anything -- there is nothing to route to.

### Teardown

```bash
cd infra && terraform destroy
```

## Benchmarks

Real, measured numbers from `scripts/run_benchmarks.py`,
`scripts/run_saturation_sweep.py`, and
`./build-release/benchmarks/edgeflow-benchmarks` -- see `docs/benchmarks.md`
and `docs/saturation.md` for the full tables. Apple M-series, 10 cores,
`-DCMAKE_BUILD_TYPE=Release`, gateway and simulator sharing one machine.
Micro-benchmarks are medians of 9 repetitions; macro rows medians of 3.

### Ingestion backend (Phase 6)

The gateway can POST batches to a NestJS service that validates them and stores
events in PostgreSQL, with Redis holding batch-level idempotency keys:
`--sink=http --sink-url=http://host:port/v1/events`, or
`--sink-url=https://<distribution>.cloudfront.net/v1/events` against the
deployed stack.

- **Sustained ingest: 23,195 events/sec (Phase 6) -> 23,757 events/sec median,
  no measurable end-to-end win (Phase 9).** Phase 6's original figure was the
  median of three runs at load average 7.02 / 8.10 / 8.02 (23,195 / 23,876 /
  18,653 events/sec); its own table conditions (row count, index size) were
  never recorded, so the two headline figures are not presented as perfectly
  controlled against each other. Method both times: 500 simulated devices at
  50 events/sec for 30s into a Release gateway (`--batch-size=100
  --batch-age-ms=200`), throughput measured as rows committed divided by wall
  time, after a discarded warmup run. Phase 9 re-ran this method with the
  pre-change backend (commit `be90766`) and the post-change backend
  interleaved A,B,A,B,A,B, `events`
  truncated before every run for a controlled comparison, at load average
  2.99-4.26: before 23,810 / 23,893 / 23,774 (median **23,810**), after
  23,757 / 23,757 / 23,761 (median **23,757**) -- a **-0.2% change, i.e. no
  measurable win** at this offered rate (500 devices x 50/sec = 25,000
  events/sec offered, and both arms land within ~5% of that ceiling). This is
  despite Tasks 2-3 (dropping the redundant transaction, then naming the
  prepared statement) measuring an **18-19% latency cut in the isolated
  database write path** (`backend/scripts/bench-ingest.js`, `notxn`/`prepared`
  vs `current`, attribution below) -- a real, reproducible saving that does not
  show up end to end because the write path is a minority of the full
  per-request critical path: two Redis round trips, NDJSON parsing and HTTP
  overhead are untouched by this phase and were never in scope. A single
  supplementary run at 500 devices x 150/sec (75,000 events/sec offered,
  genuinely saturating both arms -- `batches_dropped_outbound` > 0 in both) did
  surface the win: 49,448 -> 51,292 events/sec, **+3.7%**. `insert` execution
  dominates the write path at 63-66%; fsync is only 10.8-18.1% of it (measured
  against a matched `sync`/`nosync` control), which is why Postgres/WAL sizing
  (Phase 9 Task 5) was skipped rather than attempted. Full write-up:
  `docs/superpowers/progress/2026-08-30-phase9-ingest-latency-progress.md`.
- **Concurrent HTTP sink: 50,792 -> 65,780 events/sec median at saturation,
  +29.5% (Phase 10).** Phase 9's own "Limits" section named the single-in-flight
  sink thread as the next bottleneck -- one dedicated thread waiting for each
  response before sending the next, capping throughput at `batch_size /
  round_trip_time` no matter how cheap the backend gets. Phase 10 removed that:
  the sink now drains its outbound queue with `--sink-concurrency` threads
  (default 4, matching `--workers`), each reusing one persistent connection
  instead of opening a fresh TCP connection per batch. Measured at the same
  saturating rate Phase 9's supplementary run used, 500 devices x 150/sec
  (75,000 events/sec offered) for 30s, before arm built from the pre-Task-3
  commit (`be3872f`, single-threaded sink, one connection per batch) in a
  separate `git worktree`, after arm at HEAD; interleaved A,B,A,B,A,B, `events`
  truncated before every run, one discarded warmup, load average 2.75-4.87
  across the six timed runs. Before: 49,625 / 50,792 / 51,502 (median
  **50,792**), genuinely saturated every run (`batches_dropped_outbound`
  4,487-4,911 per run -- the sink could not keep up with what was offered).
  After: 64,732 / 69,727 / 65,780 (median **65,780**), with **zero drops in all
  three after-arm runs** -- at this offered rate the sink is no longer the
  constraint that saturates the pipeline; what actually bound the after arm was
  the single-threaded load generator's own send rate sharing the same 10-core
  machine (Phase 4 documented this as a standing, machine-specific lower
  bound). So 65,780 is a floor on the concurrent sink's ceiling, not a
  measurement of it. **The +29.5% is itself sensitive to where the generator
  landed**, because with zero drops the after arm's "throughput" *is* the
  generator's offered rate (64,800 / 69,780 / 65,835 events/sec) rather than a
  sink limit: the one pair measured at essentially identical offered volume
  (2,092,474 vs 2,093,395 events sent) gives +37.3%, not +29.5%. The figure
  that does not move with generator variance is delivered/offered -- **78.2% /
  76.6% / 77.0% before, 100% / 100% / 100% after**. Read the +29.5% as the
  conservative end of a range, not a point estimate. **Against the arithmetic
  prediction:** Phase 9's supplementary run implied one thread caps out near
  ~51,300 events/sec at
  ~1.95ms/round-trip, derived from a single run per arm; Task 1's harness
  measured the round trip directly instead (median 0.278-0.757ms across three
  non-outlier runs against the live backend, `wait`-for-first-byte 73.9-88.3%
  of it, i.e. I/O-bound, which is what opened the concurrency gate). A naive 4x
  of ~51,300 would predict roughly 205,000 events/sec; the measured **+29.5%**
  is far short of that, exactly as the design spec anticipated -- once the sink
  itself stops serializing, something else becomes the limit before a clean
  N-fold gain can show up end to end, and on this run that something was the
  generator, not yet Postgres (Phase 4's ~200,000 events/sec gateway knee is
  itself a lower bound and was never approached). At Phase 6's original rate
  (500 devices x 50/sec, secondary continuity check only, headroom-capped
  ~4.8% per the design spec's own framing since the pipeline was already near
  the offered rate before this phase): before median 22,814 (22,809 / 22,814 /
  22,820), after median 22,775 (22,763 / 22,775 / 22,779), essentially flat
  (-0.17%) with zero drops in either arm -- not evidence about concurrency,
  reported only for continuity with every figure published since Phase 6. Full
  write-up:
  `docs/superpowers/progress/2026-08-31-phase10-concurrent-sink-progress.md`.
- **The ceiling is real, not an artifact of the offered load.** Tripling the
  offer to 75,000 events/sec did not move it: the backend stored 23,324 and
  22,427 events/sec on two runs at comparable load, while the gateway's
  outbound queue shed 14,625 and 14,819 batches. Across the four runs taken at
  load average below 9.4 -- at both offered rates -- ingest sat between 22,427
  and 23,876 events/sec, a 6.5% spread.
- Nothing is lost or duplicated across the language boundary: at an offered
  rate below the ceiling (500 devices at 20 events/sec), 294,064 events
  accepted by the gateway produced exactly 294,064 rows, with zero batches
  retried, dropped, or malformed.
- Replaying a batch with the same `Idempotency-Key` stores it once.

**A database-outage data-loss defect was found by the acceptance run and
fixed.** As originally built, the idempotency key was claimed before the insert
and never released when the insert failed, so 503 -> sink retries ->
`{"stored":0,"duplicate":true}` -> sink sees 2xx and drops the batch wrote
nothing while both sides reported success. It was measured, not theorised: with
PostgreSQL stopped for ~8s during a 30s run, `batches_duplicate_suppressed` rose
by exactly the 66 batches the gateway retried and ~6,600 events never landed.

A failed insert now releases the key, and a regression test asserts the retry
stores the batch. Claiming still happens *before* the insert, which is what
closes the window where a retry arriving mid-insert would store the batch twice.

Re-measured against a real stopped database, same 30s / 5,000 events-per-second
shape: `batches_duplicate_suppressed` went from 66 (exactly matching
`batches_retried`, the defect's signature) to 1 against 13 retries, and every
event not stored is now attributable to a counter the gateway reports -- 169
batches shed at the sink's outbound queue and 3 exhausted -- rather than
vanishing while both sides report success. The re-run also exposed a crash:
stopping PostgreSQL killed the backend outright via an unhandled `pg` client
error (57P01) on a checked-out connection, which is now handled.

**The narrower window Phase 6 left open is closed in Phase 7.** If the database
failed *slowly* rather than fast -- a hung insert, a lock pile-up, a failover --
the sink's 5s timeout could fire while the first attempt was still inside its
insert. The retry found the key still claimed, got
`{"stored":0,"duplicate":true}`, and dropped the batch. Closing it needed the
claim to distinguish *in flight* from *committed* rather than being a single
boolean, which is exactly what Phase 6 deferred and Phase 7 built: one atomic
`SET key inflight NX GET EX 15` both takes a lease and reports what was already
there, and the key is promoted to `committed` only once the insert has
committed. A retry arriving mid-insert now gets a retryable 503 rather than a
false `duplicate`, which is what finally makes a 200 with `"duplicate":true`
mean the rows exist. The 15s lease is a crash-recovery bound, and deliberately
not a "the retry lands before the sink gives up" mechanism -- it cannot be one.
Only the *first* attempt against a hung holder spends its full 5s timeout;
attempts 2-4 hit the in-flight path, which answers 503 in milliseconds, so the
sink exhausts at roughly 6s with about 9s still on the lease. What the lease
actually buys is that a holder which dies mid-insert stops owning the key: once
it expires, a fresh request carrying the same key can store the batch instead
of being refused for the rest of the 900s deduplication window. The batch the
sink gave up on is dropped *visibly*, under `batches_dropped_exhausted`, rather
than silently while both sides report success.

**Measured against a SIGSTOPped database, which is the only way to reproduce
it.** `docker compose stop postgres` terminates connections, so inserts fail
*fast* -- that is Phase 6's scenario and it cannot exercise this window at all.
`docker compose pause` SIGSTOPs the container, so connections **hang** and an
insert is still running when the sink's 5s timeout fires. Both were run for 30s
at 500 devices x 10 events/sec, with a 10s outage in the middle, at load average
4.15 (two unrelated CPU-heavy processes from another project were still running;
the assertions below are conservation identities, not throughput figures, so
they are robust to that -- no rate is published from these runs).

| | `pause` (slow failure) | `stop` (fast failure) |
|---|---|---|
| accepted | 148,267 | 148,291 |
| rows committed | 123,863 | 119,200 |
| `batches_in_flight_rejected` | **3** | **0** |
| `db_failures` | 0 | 50 |
| `batches_retried` | 3 | 38 |
| `batches_dropped_outbound` | 245 | 281 |
| `batches_dropped_exhausted` | 1 | 12 |
| `batches_duplicate_suppressed` | 0 | 0 |

The two columns exercise different code paths, which is the point. Under
`pause` the inserts hang: `db_failures` is 0 and the in-flight path fires.
Under `stop` they are killed outright: `db_failures` dominates. A `stop`-based
run alone would have come back green and proven nothing about this phase.

**Each scenario was then repeated three times** (four runs each including the
above), with the conservation identities checked mechanically per run rather
than by eye. All eight runs pass. The `pause` numbers are near-identical every
time -- `batches_retried` 3, `batches_in_flight_rejected` 3,
`batches_dropped_exhausted` 1 in all four -- so the mechanism is reproducible,
not a one-off.

The repetition corrected one claim this section previously made. `stop` is
*not* purely a fast-failure mode: two of its four runs produced
`batches_in_flight_rejected` of 2. `docker compose stop` sends SIGTERM and
waits for a graceful exit, so there is a brief window in which connections hang
before they die. The distinction that survives is the one that matters --
`pause` yields 0 `db_failures` and a reliable 3 in-flight rejections, `stop`
yields 21-50 `db_failures` and only incidental ones -- but "only `pause`
produces in-flight rejections" was too strong and is withdrawn.

Across all eight runs the arrival identity holds exactly:

    batches_received = batches_sent + db_failures + batches_in_flight_rejected + T

where `T` is the number of attempts that hung past the sink's timeout and
committed anyway -- 1 in every `pause` run, 0 in every `stop` run.

Nothing goes missing while both sides report success. Under `pause`, 148,267 -
123,863 = 24,404 events were not stored, against 245 + 1 = 246 batches the
gateway itself counted as dropped, which can hold at most 24,600 -- so the
shortfall is fully attributable, with no residue. `events_stored` equalled the
row count exactly in both runs. The backend's arrival count reconciles exactly
too: under `stop`, 1,197 delivered + 50 failed = 1,247 received; under `pause`,
1,245 delivered + 1 first attempt + 3 in-flight-rejected retries = 1,249.

That last identity is the new path caught in the act: one batch's first attempt
hung inside its insert, the sink timed out and retried three times, and each
retry found the lease held and got a 503 -- `batches_retried` 3 and
`batches_in_flight_rejected` 3, exactly matching. Before Phase 7 those three
retries would each have been told `duplicate: true` and the batch would have
been dropped as delivered.

Note the run produced `batches_dropped_exhausted=1`. An earlier version of this
section derived the 15s lease from the sink's ~20.7s retry budget and predicted
exhausted batches should not occur; that reasoning is withdrawn above, and the
measurement is what confirms it. The gateway's report stays conservative in the
safe direction: it never claims success for data that was not stored, though it
may report a batch as dropped whose insert did in fact commit after the database
came back.

**A Redis restart no longer costs deduplication for the life of the process.**
Restarting Redis mid-run, health went `redis: true -> false -> true` across two
consecutive polls a second apart, on the same backend PID with no process
restart, logging `connection lost` then `connection restored`. 22 claims failed
open during the gap and every one returned promptly rather than blocking, and
`GET /v1/health` kept answering throughout instead of hanging with the outage.
All 49,400 accepted events were committed -- zero dropped, zero retried.

**PostgreSQL is now the pipeline's narrowest point, by roughly 8.6x.** The
gateway ingests about 200,000 events/sec (Phase 4) and this path commits about
23,200, so the C++ side is no longer the constraint by a wide margin -- one
synchronous service doing multi-row `INSERT`s inside a transaction is. Past the
ceiling the loss is orderly and visible rather than silent: the sink's
256-batch outbound queue fills and increments `batches_dropped_outbound`, which
is what the 14,625-batch figure above is counting. Nothing was ever retried or
exhausted, so the backend was returning 200s the whole time -- it was simply
returning them more slowly than the gateway produced work.

These numbers are a floor rather than a specification. PostgreSQL and Redis are
local containers on the same ten cores as the gateway and the load generator,
so they measure the cost of this ingestion path on one machine, not the
capacity of a deployed system. No tuning was attempted: default
`postgres:16-alpine` settings, one connection pool, no batching across
requests, no partitioning, no `COPY`.

### Gateway saturation (Phase 4)

**The gateway saturates at roughly 200,000 events/sec offered** on this
machine. Below that both queues accept everything: 196,596 events/sec
accepted at the 200,000/sec rung with zero drops and ~40us mean queue wait.
Above it the pipeline stops keeping up -- accepted falls below sent and mean
queue wait jumps by an order of magnitude (360us for the mutex queue, 4,241us
for the lock-free one at the 400,000/sec rung).

Saturation shows up as **queue-wait blowing out, not as drops**. Under the
default `--backpressure=block` the connection stops reading and retries rather
than discarding, so `dropped_oldest`/`dropped_newest` stay at zero even when
the gateway is well past its limit. A sweep that only watched drop counters
would report "never saturated" forever; `scripts/run_saturation_sweep.py`
classifies a rung on queue-wait and on `accepted < sent` as well.

Two load-generator findings, both measured:

- **More generator threads make things worse here.** At 1000 devices x 400/sec:
  1 thread sends 371,506/sec, 2 threads 338,451/sec, 3 threads 71,662/sec.
  Generator and gateway share ten cores, so extra generator threads take cores
  the gateway needs. `--threads` exists and works; on one machine, leave it at 1.
- **The send interval is computed in microseconds.** It used to be whole
  milliseconds, which silently rounded any rate that did not divide 1000 evenly
  *upwards* -- 400/sec became 500/sec, 800/sec became 1000/sec -- capping the
  fleet near 500,000 events/sec and making the ladder's top rungs measure
  demand nobody asked for.

The disk is not the constraint: re-running the whole ladder with
`--sink-file=/dev/null` changed accepted throughput by under 5% on nine of
twelve rungs, with zero drops either way.

Because generator and gateway share one machine, the knee is a **lower bound**.
A dedicated load box would very likely push it higher.

### HTTP sink (Phase 5)

The gateway can POST batches to an HTTP endpoint instead of writing them to a
file: `--sink=http --sink-url=http://host:port/path`. The body is NDJSON,
**byte-identical to what `FileSink` writes**, so a file can be replayed to the
backend with plain `curl`.

`https://` URLs are supported and terminate TLS in the sink itself: the server
certificate is verified against the system trust store, the hostname is checked
against it, and SNI is sent so a multi-tenant edge such as CloudFront can select
a certificate. The port defaults to 443 for `https://` and 80 for `http://`. An
unverifiable certificate fails the batch -- there is no downgrade to cleartext.
This is what lets the gateway reach the AWS deployment, whose only entry point
is HTTPS.

- **End to end, nothing is lost while the backend is healthy.** 1000 devices at
  50 events/sec for 20s: 938,191 events accepted, 9,430 batches sent, and the
  backend received exactly 938,191 lines. Zero retried, zero dropped.
- **The network sink does not move the saturation knee.** Re-running Phase 4's
  full ladder against a local HTTP backend and against the file sink,
  back to back at load average ~2.3, three repetitions each: accepted
  throughput differed by under 1% on eight of twelve rungs, with zero drops
  under either sink. The two larger gaps (-9.9% and -30.5%) both fall on
  rungs already classified generator-limited, where the load generator rather
  than the sink is the variable.
- Mean queue wait stayed at ~5.6us with the HTTP sink under a 20-device load,
  confirming worker threads are not blocking on the network -- `consume()`
  queues the batch and returns, and a dedicated thread does the round trips.

Four counters on the shutdown line account for every batch: `batches_sent`,
`batches_retried` (attempts, not batches), `batches_dropped_outbound` (the
outbound queue was full) and `batches_dropped_exhausted` (retries ran out, or
the 5s shutdown drain deadline passed). The two drop counters are separate so a
backend outage reads differently from a traffic burst.

**The sink speaks plain HTTP with no TLS and no authentication.** It is for a
trusted network or a local backend only; both belong with the service itself,
which is a later phase.

### Queue comparison (Phase 3, unchanged by Phase 4)

- SPMC micro-benchmark (one producer, N consumers -- the shape the gateway
  actually has): mutex 82.2 / 158 / 289 / 1308 ns per push at 1/2/4/8
  consumers, lock-free 53.2 / 53.1 / 138 / 205 ns. The lock-free queue is
  1.6x faster at one consumer and 6.4x at eight.
- Below the knee the two are indistinguishable end to end. **Above it the
  mutex queue wins**, which is the opposite of what the micro-benchmark
  predicts: at the 400,000/sec rung the mutex gateway sustained 177,737
  events/sec at 361us mean queue wait, the lock-free one 114,794 at 4,241us.
  Worth treating as provisional -- it is three repetitions on a shared machine,
  and the two differ by more past the knee than anything measured below it.
- `BoundedQueue` (mutex) stays the production default. Three phases have now
  failed to find a load at which it is the bottleneck, and the one regime where
  the two measurably diverge favours it.

Reproduce: build with `-DCMAKE_BUILD_TYPE=Release`, then
`python3 scripts/run_saturation_sweep.py --build-dir build-release`
(around ten minutes) and `python3 scripts/run_benchmarks.py --build-dir build-release`.
Run them on an otherwise idle machine -- throughput here swings several-fold
with background load.
