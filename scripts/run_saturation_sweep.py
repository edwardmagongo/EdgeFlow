#!/usr/bin/env python3
"""Walks a ladder of offered event rates against both gateway binaries and
reports where -- and whether -- the gateway saturates.

Each rung is classified, because "the generator fell short" and "the gateway
saturated" both look like accepted < offered:

  * generator-limited : the simulator sent less than 95% of the target, AND
                        queue-wait stayed close to this queue's own baseline.
                        The rung says nothing about the gateway and is
                        excluded from the knee.
  * gateway-limited   : the simulator sent less than 95% of the target, but
                        queue_wait_p99_us was sharply elevated relative to
                        this queue's own lowest-rung baseline. The gateway is
                        backpressuring the simulator (TCP-level pushback under
                        --backpressure=block), which is itself evidence the
                        gateway is the bottleneck -- see the note below.
  * saturated         : the simulator hit its target and the gateway's own
                        drop counters are nonzero. The gateway is the
                        bottleneck. NOTE: this script always runs the gateway
                        with --backpressure=block, under which push() only
                        ever returns Accepted or RejectedBackpressure --
                        dropped_oldest/dropped_newest require the DropOldest/
                        DropNewest policy, which this script never selects.
                        So `saturated` is not reachable in practice today; it
                        is kept as a classification (rather than removed) so
                        the script stays correct if the backpressure policy is
                        ever made configurable. `gateway-limited` is the
                        signal that actually fires under Block.
  * clean             : target met, nothing dropped.

The knee is the highest clean rung. A queue with `gateway-limited` rows is
more informative than one with only `generator-limited` rows: it means the
gateway measurably saturated even though nothing was dropped.

Usage:
    python3 scripts/run_saturation_sweep.py [--build-dir build-release]
        [--repetitions 3] [--devices 1000] [--duration 10]
        [--queue-capacity 8192] [--sink-file /tmp/edgeflow_saturation.ndjson]
"""
import argparse
import csv
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_benchmarks import SHUTDOWN_RE  # noqa: E402  (path set above)

# Matches the simulator's completion line from Task 3:
#   edgeflow-simulator: done (1000 total devices, 1957123 events sent)
SENT_RE = re.compile(r"(?P<sent>\d+) events sent")

# Offered events/sec. The fleet is held constant and per-device rate is scaled,
# so socket count stays fixed and event rate is the only variable.
LADDER = [25_000, 50_000, 100_000, 200_000, 400_000, 800_000]

# How much a rung's queue_wait_p99_us must exceed that queue's own baseline
# (the queue_wait_p99_us of its LADDER[0] rung, the least-contended point on
# the ladder) before a generator-limited rung is reclassified as
# gateway-limited. An order-of-magnitude increase is a strong signal of real
# queueing delay building up inside the gateway, not just measurement jitter
# at low load (baseline p99s on an idle-ish queue are typically single- to
# low-double-digit microseconds, so ordinary noise would need a very unlucky
# sample to cross a 10x bar). The comparison is baseline-relative per queue
# per run, not an absolute microsecond constant, so it self-calibrates across
# machines and queue implementations instead of relying on one fixed cutoff.
GATEWAY_LIMITED_MULTIPLIER = 10

FIELDNAMES = [
    "target_events_per_sec", "queue", "devices", "rate_per_device", "duration_s",
    "queue_capacity", "events_sent", "achieved_send_rate", "accepted",
    "accepted_rate", "dropped", "queue_wait_mean_us", "queue_wait_p99_us",
    "classification", "error",
]


def run_rung(gateway_bin, simulator_bin, sink_file, target, queue, *,
             devices, duration, queue_capacity, port):
    """One rung, one queue. Returns a dict of measurements."""
    rate = target / devices
    result = {
        "target_events_per_sec": target, "queue": queue, "devices": devices,
        "rate_per_device": rate, "duration_s": duration,
        "queue_capacity": queue_capacity, "error": "",
    }

    # Only regular files are removed between runs. The disk-isolation control
    # points the sink at /dev/null, which exists and must not be unlinked.
    if sink_file.is_file():
        sink_file.unlink()
    gateway = subprocess.Popen(
        [str(gateway_bin), f"--port={port}", f"--queue-capacity={queue_capacity}",
         "--workers=4", "--backpressure=block", f"--sink-file={sink_file}"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.5)  # let the acceptor bind before the fleet connects

    try:
        sim = subprocess.run(
            [str(simulator_bin), f"--port={port}", f"--devices={devices}",
             f"--rate={rate}", f"--duration={duration}"],
            capture_output=True, text=True, timeout=duration + 60)
        if sim.returncode != 0:
            result["error"] = f"simulator exited {sim.returncode}: {sim.stderr.strip()}"
    except subprocess.TimeoutExpired:
        result["error"] = "simulator timed out"
        sim = None

    gateway.terminate()
    try:
        stdout, _ = gateway.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        gateway.kill()
        stdout, _ = gateway.communicate()
        if not result["error"]:
            result["error"] = "gateway did not shut down within 30s of SIGTERM"

    if result["error"]:
        return result

    sent_match = SENT_RE.search(sim.stdout)
    shutdown_match = SHUTDOWN_RE.search(stdout)
    if not sent_match:
        result["error"] = f"could not parse simulator events-sent line: {sim.stdout!r}"
        return result
    if not shutdown_match:
        result["error"] = f"could not parse gateway shutdown line: {stdout!r}"
        return result

    groups = shutdown_match.groupdict()
    sent = int(sent_match.group("sent"))
    accepted = int(groups["accepted"])
    dropped = int(groups["dropped_oldest"]) + int(groups["dropped_newest"])

    result.update({
        "events_sent": sent,
        "achieved_send_rate": round(sent / duration, 1),
        "accepted": accepted,
        "accepted_rate": round(accepted / duration, 1),
        "dropped": dropped,
        "queue_wait_mean_us": float(groups["queue_wait_mean_us"]),
        "queue_wait_p99_us": int(groups["queue_wait_p99_us"]),
    })

    # Classification. The generator check comes first: if the simulator never
    # offered the load, the gateway's behaviour at this rung is meaningless
    # *unless* queue-wait says otherwise -- that reclassification needs the
    # whole sweep's baseline rungs, so it happens later, in
    # reclassify_gateway_limited(), once every rung has been run.
    if sent < 0.95 * target * duration:
        result["classification"] = "generator-limited"
    elif dropped > 0:
        result["classification"] = "saturated"
    else:
        result["classification"] = "clean"
    return result


def median_of(results, key):
    values = [r[key] for r in results if not r["error"] and key in r]
    return round(statistics.median(values), 1) if values else ""


def aggregate(reps):
    """Collapse repetitions of one (rung, queue) into a median row."""
    ok = [r for r in reps if not r["error"]]
    if not ok:
        return reps[0]
    row = dict(ok[0])
    for key in ("events_sent", "achieved_send_rate", "accepted", "accepted_rate",
                "dropped", "queue_wait_mean_us", "queue_wait_p99_us"):
        row[key] = median_of(ok, key)
    # Worst-case classification wins: a rung that saturated in any repetition is
    # not "clean", and the knee must not be reported past a rung that ever dropped.
    classes = {r["classification"] for r in ok}
    for label in ("generator-limited", "saturated", "clean"):
        if label in classes:
            row["classification"] = label
            break
    return row


def reclassify_gateway_limited(rows, baseline_target, multiplier=GATEWAY_LIMITED_MULTIPLIER):
    """Second pass over the aggregated rows: turn generator-limited rungs into
    gateway-limited where queue-wait says the gateway, not the simulator, is
    the reason offered load fell short.

    Runs after the whole sweep (not inside run_rung) because the baseline is
    each queue's own LADDER[0] rung, which isn't known as an aggregated median
    until that rung's repetitions have all been collapsed.
    """
    queues = {r["queue"] for r in rows if not r.get("error")}
    for queue in queues:
        baseline_row = next(
            (r for r in rows if r["queue"] == queue
             and r["target_events_per_sec"] == baseline_target and not r.get("error")),
            None)
        if baseline_row is None:
            continue  # no usable baseline for this queue; leave as generator-limited
        baseline_qwait = baseline_row.get("queue_wait_p99_us")
        if not isinstance(baseline_qwait, (int, float)):
            continue
        # Floor the baseline at 1us so a queue that happened to see zero
        # queue-wait at the lowest rung doesn't make the multiplier trivially
        # satisfied by any nonzero later reading.
        baseline_qwait = max(baseline_qwait, 1)
        for row in rows:
            if (row["queue"] != queue or row.get("error")
                    or row.get("classification") != "generator-limited"):
                continue
            qwait = row.get("queue_wait_p99_us")
            if isinstance(qwait, (int, float)) and qwait >= multiplier * baseline_qwait:
                row["classification"] = "gateway-limited"


def write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDNAMES)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in FIELDNAMES})


def write_markdown(path, rows, knees):
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# EdgeFlow Gateway Saturation Curve",
        "",
        "Generated by `scripts/run_saturation_sweep.py`. Every figure is the median",
        "of repeated runs of the real gateway and simulator. Fully overwritten on",
        "each run.",
        "",
        "A rung is only a valid statement about the gateway when the simulator",
        "actually offered the load: `generator-limited` rungs are excluded from the",
        "knee, because they measure the load generator, not the gateway.",
        "",
        "This sweep always runs the gateway with `--backpressure=block`, under which",
        "the drop counters never increment (`saturated` is unreachable in practice",
        "under Block). A gateway that is genuinely saturated instead shows up as",
        "`gateway-limited`: offered load fell short (same size check as",
        f"`generator-limited`) AND `queue_wait_p99_us` was at least "
        f"{GATEWAY_LIMITED_MULTIPLIER}x that queue's own baseline (its lowest rung, "
        f"target={LADDER[0]:,} events/sec) -- i.e. real queueing delay, not generator jitter.",
        "",
    ]
    for queue, knee in knees.items():
        lines.append(f"- **{queue}**: {knee}")
    lines += [
        "",
        "| target ev/s | queue | sent ev/s | accepted ev/s | dropped | "
        "qwait mean (us) | qwait p99 (us) | classification |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        lines.append(
            f"| {row.get('target_events_per_sec','')} | {row.get('queue','')} | "
            f"{row.get('achieved_send_rate','')} | {row.get('accepted_rate','')} | "
            f"{row.get('dropped','')} | {row.get('queue_wait_mean_us','')} | "
            f"{row.get('queue_wait_p99_us','')} | "
            f"{row.get('classification','') or row.get('error','')} |")
    path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-release")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--devices", type=int, default=1000)
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--queue-capacity", type=int, default=8192)
    parser.add_argument("--sink-file", default="/tmp/edgeflow_saturation.ndjson")
    parser.add_argument("--out-csv", default="benchmarks/saturation.csv")
    parser.add_argument("--out-md", default="docs/saturation.md")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    build_dir = (repo_root / args.build_dir).resolve()
    gateway_bins = {
        "mutex": build_dir / "gateway" / "edgeflow-gateway",
        "lock-free": build_dir / "gateway" / "edgeflow-gateway-lockfree",
    }
    simulator_bin = build_dir / "simulator" / "edgeflow-simulator"
    missing = [str(p) for p in list(gateway_bins.values()) + [simulator_bin]
               if not p.exists()]
    if missing:
        print("error: build the binaries first (missing: " + ", ".join(missing) + ")",
              file=sys.stderr)
        return 1

    sink_file = Path(args.sink_file)
    rows = []
    for target in LADDER:
        # Queues interleaved inside the rung so both see the same machine
        # conditions -- see the note at the top of this task.
        for queue, gateway_bin in gateway_bins.items():
            reps = []
            for rep in range(args.repetitions):
                print(f"running: target={target} queue={queue} rep={rep + 1}/"
                      f"{args.repetitions} ...", file=sys.stderr)
                reps.append(run_rung(
                    gateway_bin, simulator_bin, sink_file, target, queue,
                    devices=args.devices, duration=args.duration,
                    queue_capacity=args.queue_capacity, port=19500))
            row = aggregate(reps)
            if row["error"]:
                print(f"  FAILED: {row['error']}", file=sys.stderr)
            else:
                print(f"  sent={row['achieved_send_rate']}/s "
                      f"accepted={row['accepted_rate']}/s dropped={row['dropped']} "
                      f"-> {row['classification']}", file=sys.stderr)
            rows.append(row)

    reclassify_gateway_limited(rows, baseline_target=LADDER[0])

    knees = {}
    for queue in gateway_bins:
        q_rows = [r for r in rows if r["queue"] == queue]
        clean = [r for r in q_rows if r.get("classification") == "clean"]
        saturated = [r for r in q_rows if r.get("classification") == "saturated"]
        gateway_limited = [r for r in q_rows if r.get("classification") == "gateway-limited"]

        # Guard every max()/min() with default=None: a queue can now have
        # saturated or gateway-limited rows with zero clean rows (e.g. it
        # never once ran clean before hitting trouble), and a queue where
        # every valid rung came back gateway-limited has zero clean AND zero
        # saturated rows. Both used to be unreachable (saturated never fired
        # under Block) but gateway-limited makes them live paths.
        clean_max = max((r["target_events_per_sec"] for r in clean), default=None)

        if saturated:
            saturated_min = min((r["target_events_per_sec"] for r in saturated), default=None)
            if clean_max is not None:
                knees[queue] = (f"knee at {clean_max} events/sec offered; saturates "
                                f"(drops events) by {saturated_min} events/sec offered")
            else:
                knees[queue] = (f"gateway drops events starting at {saturated_min} "
                                f"events/sec offered; no clean rung was observed below it "
                                f"to report a knee from.")
        elif gateway_limited:
            # No drops (expected under --backpressure=block), but queue-wait
            # climbing >= GATEWAY_LIMITED_MULTIPLIER x baseline is still a real
            # saturation signal -- more informative than a bare
            # generator-limited row would have been, so call it out plainly.
            gw_min = min((r["target_events_per_sec"] for r in gateway_limited), default=None)
            if clean_max is not None:
                knees[queue] = (f"knee at {clean_max} events/sec offered; queue-wait signal "
                                f"shows the gateway saturating (no drops under Block policy) "
                                f"starting at {gw_min} events/sec offered")
            else:
                knees[queue] = (f"every valid rung was gateway-limited: queue_wait_p99_us "
                                f"was >= {GATEWAY_LIMITED_MULTIPLIER}x baseline starting at "
                                f"{gw_min} events/sec offered, with no clean rung observed to "
                                f"report a knee from.")
        elif clean:
            knees[queue] = (f"never saturated: clean at every valid rung up to "
                            f"{clean_max} events/sec offered. The ceiling is above this "
                            f"and was not reached.")
        else:
            knees[queue] = "no valid rung: every rung was generator-limited or failed."

    write_csv(repo_root / args.out_csv, rows)
    write_markdown(repo_root / args.out_md, rows, knees)
    for queue, knee in knees.items():
        print(f"{queue}: {knee}", file=sys.stderr)
    print(f"wrote {args.out_csv} and {args.out_md}", file=sys.stderr)
    return 1 if any(r["error"] for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
