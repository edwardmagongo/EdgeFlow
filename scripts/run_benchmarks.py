#!/usr/bin/env python3
"""Runs edgeflow-gateway + edgeflow-simulator across a configuration matrix,
parses the gateway's shutdown line for its Stats counters, and writes results
to a CSV plus a regenerated markdown table.

Usage:
    python3 scripts/run_benchmarks.py [--build-dir build] \
        [--out-csv benchmarks/results.csv] [--out-md docs/benchmarks.md]
"""
import argparse
import csv
import re
import subprocess
import sys
import time
from pathlib import Path

SHUTDOWN_RE = re.compile(
    r"accepted=(?P<accepted>\d+), dropped_oldest=(?P<dropped_oldest>\d+), "
    r"dropped_newest=(?P<dropped_newest>\d+), malformed=(?P<malformed>\d+), "
    r"queue_wait_count=(?P<queue_wait_count>\d+), "
    r"queue_wait_mean_us=(?P<queue_wait_mean_us>[\d.]+), "
    r"queue_wait_p50_us=(?P<queue_wait_p50_us>\d+), "
    r"queue_wait_p99_us=(?P<queue_wait_p99_us>\d+)\)"
)

FIELDNAMES = [
    "label", "queue", "workers", "queue_capacity", "backpressure", "devices", "rate",
    "duration_s", "chaos_latency_ms", "chaos_packet_loss_percent",
    "chaos_device_spike", "chaos_device_spike_at_sec",
    "accepted", "dropped_oldest", "dropped_newest", "malformed",
    "throughput_events_per_sec", "queue_wait_count", "queue_wait_mean_us",
    "queue_wait_p50_us", "queue_wait_p99_us", "error",
]


def run_one(gateway_bin, simulator_bin, sink_file, label, *, queue="mutex",
            workers=4, queue_capacity=2048, backpressure="block",
            devices=200, rate=10.0, duration_s=10,
            chaos_latency_ms=0, chaos_packet_loss_percent=0.0,
            chaos_device_spike=0, chaos_device_spike_at_sec=0):
    sink_file.unlink(missing_ok=True)

    gateway_cmd = [
        str(gateway_bin),
        "--port=19100",
        f"--workers={workers}",
        f"--queue-capacity={queue_capacity}",
        f"--backpressure={backpressure}",
        f"--sink-file={sink_file}",
    ]
    gateway = subprocess.Popen(gateway_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.3)  # let the gateway finish binding the port before the simulator connects

    simulator_cmd = [
        str(simulator_bin),
        "--port=19100",
        f"--devices={devices}",
        f"--rate={rate}",
        f"--duration={duration_s}",
    ]
    if chaos_latency_ms:
        simulator_cmd.append(f"--chaos-latency-ms={chaos_latency_ms}")
    if chaos_packet_loss_percent:
        simulator_cmd.append(f"--chaos-packet-loss-percent={chaos_packet_loss_percent}")
    if chaos_device_spike:
        simulator_cmd.append(f"--chaos-device-spike={chaos_device_spike}")
        simulator_cmd.append(f"--chaos-device-spike-at-sec={chaos_device_spike_at_sec}")

    result = {
        "label": label, "queue": queue, "workers": workers, "queue_capacity": queue_capacity,
        "backpressure": backpressure, "devices": devices, "rate": rate,
        "duration_s": duration_s, "chaos_latency_ms": chaos_latency_ms,
        "chaos_packet_loss_percent": chaos_packet_loss_percent,
        "chaos_device_spike": chaos_device_spike,
        "chaos_device_spike_at_sec": chaos_device_spike_at_sec, "error": "",
    }

    try:
        sim_proc = subprocess.run(simulator_cmd, capture_output=True, text=True, timeout=duration_s + 15)
        if sim_proc.returncode != 0:
            result["error"] = f"simulator exited {sim_proc.returncode}: {sim_proc.stderr.strip()}"
    except subprocess.TimeoutExpired:
        result["error"] = "simulator timed out"

    gateway.terminate()
    try:
        stdout, _ = gateway.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        gateway.kill()
        stdout, _ = gateway.communicate()
        if not result["error"]:
            result["error"] = "gateway did not shut down within 10s of SIGTERM"

    if not result["error"]:
        match = SHUTDOWN_RE.search(stdout)
        if not match:
            result["error"] = f"could not parse gateway shutdown line: {stdout!r}"
        else:
            groups = match.groupdict()
            result["accepted"] = int(groups["accepted"])
            result["dropped_oldest"] = int(groups["dropped_oldest"])
            result["dropped_newest"] = int(groups["dropped_newest"])
            result["malformed"] = int(groups["malformed"])
            result["queue_wait_count"] = int(groups["queue_wait_count"])
            result["queue_wait_mean_us"] = float(groups["queue_wait_mean_us"])
            result["queue_wait_p50_us"] = int(groups["queue_wait_p50_us"])
            result["queue_wait_p99_us"] = int(groups["queue_wait_p99_us"])
            result["throughput_events_per_sec"] = round(result["accepted"] / duration_s, 1)

    return result


def build_matrix():
    rows = []
    for workers in (1, 2, 4, 8):
        rows.append(dict(label=f"workers={workers}", workers=workers))
    # Powers of two so the mutex and lock-free queues hold exactly the same number
    # of items. The lock-free ring rounds capacity up to a power of two, so the old
    # 100 / 1000 / 10000 / 50 rows would have compared different capacities.
    for capacity in (128, 1024, 8192):
        rows.append(dict(label=f"queue_capacity={capacity}", queue_capacity=capacity))
    for policy in ("block", "drop-oldest", "drop-newest"):
        rows.append(dict(label=f"backpressure={policy}", backpressure=policy,
                          queue_capacity=64, devices=500, rate=50.0))
    rows.append(dict(label="baseline-for-chaos", devices=100, duration_s=15))
    rows.append(dict(label="chaos-latency", devices=100, duration_s=15, chaos_latency_ms=200))
    rows.append(dict(label="chaos-packet-loss", devices=100, duration_s=15, chaos_packet_loss_percent=20.0))
    rows.append(dict(label="chaos-device-spike", devices=100, duration_s=15,
                      chaos_device_spike=100, chaos_device_spike_at_sec=5))
    return rows


def write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDNAMES)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in FIELDNAMES})


def write_markdown(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# EdgeFlow Phase 3 Benchmark Results",
        "",
        "Generated by `scripts/run_benchmarks.py`. Real measured numbers from an",
        "actual run of `edgeflow-gateway` + `edgeflow-simulator` -- nothing here is",
        "invented. Re-run the script to regenerate this file; it is fully",
        "overwritten on each run.",
        "",
        "| label | queue | workers | queue_capacity | backpressure | devices | rate | "
        "throughput (events/s) | queue_wait p50 (us) | queue_wait p99 (us) | "
        "dropped_oldest | dropped_newest | error |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        lines.append(
            f"| {row.get('label','')} | {row.get('queue','')} | {row.get('workers','')} | "
            f"{row.get('queue_capacity','')} | {row.get('backpressure','')} | "
            f"{row.get('devices','')} | {row.get('rate','')} | "
            f"{row.get('throughput_events_per_sec','')} | "
            f"{row.get('queue_wait_p50_us','')} | {row.get('queue_wait_p99_us','')} | "
            f"{row.get('dropped_oldest','')} | {row.get('dropped_newest','')} | "
            f"{row.get('error','')} |"
        )
    path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--out-csv", default="benchmarks/results.csv")
    parser.add_argument("--out-md", default="docs/benchmarks.md")
    parser.add_argument("--sink-file", default="/tmp/edgeflow_benchmark_sink.ndjson")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    build_dir = (repo_root / args.build_dir).resolve()
    # One binary per queue implementation; they differ only in a compile
    # definition (see gateway/CMakeLists.txt).
    gateway_bins = {
        "mutex": build_dir / "gateway" / "edgeflow-gateway",
        "lock-free": build_dir / "gateway" / "edgeflow-gateway-lockfree",
    }
    simulator_bin = build_dir / "simulator" / "edgeflow-simulator"
    missing = [str(path) for path in list(gateway_bins.values()) + [simulator_bin]
               if not path.exists()]
    if missing:
        print("error: build the gateway/simulator binaries first (missing: "
              + ", ".join(missing) + ")", file=sys.stderr)
        return 1

    sink_file = Path(args.sink_file)
    rows = []
    for queue_name, gateway_bin in gateway_bins.items():
        for row_config in build_matrix():
            label = row_config["label"]
            print(f"running: queue={queue_name} {label} ...", file=sys.stderr)
            result = run_one(gateway_bin, simulator_bin, sink_file,
                             queue=queue_name, **row_config)
            if result["error"]:
                print(f"  FAILED: {result['error']}", file=sys.stderr)
            else:
                print(f"  throughput={result['throughput_events_per_sec']} events/s, "
                      f"p50={result['queue_wait_p50_us']}us, p99={result['queue_wait_p99_us']}us",
                      file=sys.stderr)
            rows.append(result)

    write_csv(repo_root / args.out_csv, rows)
    write_markdown(repo_root / args.out_md, rows)
    print(f"wrote {args.out_csv} and {args.out_md}", file=sys.stderr)

    failures = [r for r in rows if r["error"]]
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
