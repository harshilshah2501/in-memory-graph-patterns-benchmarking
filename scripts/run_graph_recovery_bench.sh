#!/usr/bin/env bash
set -euo pipefail

BENCH_BINARY="${BENCH_BINARY:-./build/graph_recovery_bench}"
EDGES="${EDGES:-5000000}"
VERTEX_FACTOR="${VERTEX_FACTOR:-8}"
DISTRIBUTION="${DISTRIBUTION:-uniform}"
SEED="${SEED:-20261021}"
TRIALS="${TRIALS:-3}"
WARMUPS="${WARMUPS:-1}"
OUTPUT_ROOT="${OUTPUT_ROOT:-./artifacts/phase3-recovery}"

if [[ "${1:-}" == "--help" ]]; then
  cat <<'EOF'
Usage: run_graph_recovery_bench.sh

Environment:
  BENCH_BINARY   Recovery executable (default ./build/graph_recovery_bench)
  EDGES          Edge count, up to 40000000 (default 5000000)
  VERTEX_FACTOR  vertices = edges / VERTEX_FACTOR (default 8)
  DISTRIBUTION   uniform, hub, or clustered (default uniform)
  SEED           Dataset seed (default 20261021)
  TRIALS         Timed recovery trials per process (default 3)
  WARMUPS        Untimed recovery iterations per process (default 1)
  OUTPUT_ROOT    Output directory root (default ./artifacts/phase3-recovery)

Each candidate and operation runs in its own process. Cache conditions are:
  warm-process-restart   New process; OS page cache is not cleared.
  best-effort-file-advice POSIX_FADV_DONTNEED hint before each read; not guaranteed cold.
EOF
  exit 0
fi

if [[ ! -x "$BENCH_BINARY" ]]; then
  echo "Recovery benchmark not found or not executable: $BENCH_BINARY" >&2
  echo "Build it first with: make build/graph_recovery_bench" >&2
  exit 2
fi
if ! [[ "$EDGES" =~ ^[0-9]+$ ]] || (( 10#$EDGES == 0 || 10#$EDGES > 40000000 )); then
  echo "EDGES must be between 1 and 40000000" >&2
  exit 2
fi
if ! [[ "$VERTEX_FACTOR" =~ ^[0-9]+$ ]] || (( 10#$VERTEX_FACTOR == 0 )); then
  echo "VERTEX_FACTOR must be a positive integer" >&2
  exit 2
fi
if ! [[ "$SEED" =~ ^[0-9]+$ ]] || (( 10#$SEED > 4294967295 )); then
  echo "SEED must fit uint32" >&2
  exit 2
fi
if ! [[ "$TRIALS" =~ ^[0-9]+$ ]] || (( 10#$TRIALS == 0 )); then
  echo "TRIALS must be a positive integer" >&2
  exit 2
fi
if ! [[ "$WARMUPS" =~ ^[0-9]+$ ]]; then
  echo "WARMUPS must be a non-negative integer" >&2
  exit 2
fi
vertices=$(( 10#$EDGES / 10#$VERTEX_FACTOR ))
if (( vertices < 1 )); then
  vertices=1
fi

timestamp="$(date +%Y%m%d-%H%M%S%N)"
run_dir="$OUTPUT_ROOT/$timestamp"
mkdir -p "$run_dir"

common_args=(
  --vertices "$vertices"
  --edges "$EDGES"
  --distribution "$DISTRIBUTION"
  --seed "$SEED"
  --trials "$TRIALS"
  --warmups "$WARMUPS"
  --output-dir "$run_dir"
)

"$BENCH_BINARY" --mode prepare --candidate all "${common_args[@]}" \
  >"$run_dir/prepare.log" 2>"$run_dir/prepare.stderr"

read_rss_metrics() {
  local time_file="$1"
  local peak elapsed
  peak="$(awk -F: '/Maximum resident set size/ {gsub(/^[ \t]+/, "", $2); print $2}' "$time_file" | tail -n 1)"
  elapsed="$(awk '
    /Elapsed \(wall clock\) time/ {
      value = $NF
      count = split(value, part, ":")
      if (count == 3) seconds = part[1] * 3600 + part[2] * 60 + part[3]
      else if (count == 2) seconds = part[1] * 60 + part[2]
      else seconds = part[1]
      printf "%.2f\n", seconds
    }
  ' "$time_file" | tail -n 1)"
  if [[ -z "$peak" || -z "$elapsed" ]]; then
    echo "Could not read peak RSS or elapsed time from $time_file" >&2
    return 1
  fi
  printf '%s %s\n' "$peak" "$elapsed"
}

for candidate in forward_hash bidirectional_hash csr_csc; do
  for cache_state in warm-process-restart best-effort-file-advice; do
    for operation in source-rebuild snapshot-load; do
      result_name="${operation}_${candidate}_${cache_state}"
      time_file="$run_dir/$result_name.time"
      /usr/bin/time -v "$BENCH_BINARY" \
        --mode "$operation" \
        --candidate "$candidate" \
        --cache-state "$cache_state" \
        "${common_args[@]}" \
        >"$run_dir/$result_name.log" 2>"$time_file"
      metrics="$(read_rss_metrics "$time_file")"
      printf '%s,%s,%s,%s,%s\n' \
        "$operation" "$candidate" "$cache_state" \
        "${metrics%% *}" "${metrics##* }" >>"$run_dir/process_metrics.csv"
    done
  done
done

export RECOVERY_RUN_DIR="$run_dir"
export RECOVERY_TIMESTAMP="$timestamp"
export RECOVERY_EDGES="$EDGES"
export RECOVERY_VERTICES="$vertices"
export RECOVERY_VERTEX_FACTOR="$VERTEX_FACTOR"
export RECOVERY_DISTRIBUTION="$DISTRIBUTION"
export RECOVERY_SEED="$SEED"
export RECOVERY_TRIALS="$TRIALS"
export RECOVERY_WARMUPS="$WARMUPS"
export RECOVERY_BENCH_BINARY="$BENCH_BINARY"
python3 - <<'PY'
import csv
import json
import os
import pathlib
import platform

run_dir = pathlib.Path(os.environ["RECOVERY_RUN_DIR"])
candidates = ("forward_hash", "bidirectional_hash", "csr_csc")
cache_states = ("warm-process-restart", "best-effort-file-advice")
operations = ("source-rebuild", "snapshot-load")
settings = ("edges", "vertices", "distribution", "seed", "trials", "warmups")
process_metrics = {}
with (run_dir / "process_metrics.csv").open(newline="", encoding="utf-8") as stream:
    for operation, candidate, cache_state, rss, elapsed in csv.reader(stream):
        process_metrics[(operation, candidate, cache_state)] = (int(rss), float(elapsed))

prepare_rows = {}
with (run_dir / "prepare_results.csv").open(newline="", encoding="utf-8") as stream:
    for row in csv.DictReader(stream):
        prepare_rows[row["candidate"]] = row

raw_rows = {}
for candidate in candidates:
    for cache_state in cache_states:
        for operation in operations:
            path = run_dir / f"{operation}_{candidate}_{cache_state}_results.csv"
            with path.open(newline="", encoding="utf-8") as stream:
                row = next(csv.DictReader(stream))
            if row["equivalence"] != "PASS":
                raise ValueError(f"equivalence failed for {candidate}/{cache_state}/{operation}")
            if row["candidate"] != candidate or row["cache_state"] != cache_state:
                raise ValueError("raw recovery result identity mismatch")
            raw_rows[(operation, candidate, cache_state)] = row

columns = (
    "schema_version", "operation", "candidate", "cache_state", "edges",
    "vertices", "distribution", "seed", "trials", "warmups",
    "ready_min_ms", "ready_p50_ms", "ready_p95_ms", "ready_mean_ms",
    "ready_max_ms", "expected_checksum", "restored_checksum", "equivalence",
    "peak_rss_kb", "process_elapsed_seconds", "source_bytes", "snapshot_bytes",
    "source_write_ms", "snapshot_write_ms",
    "source_rebuild_ready_ms_p50", "snapshot_load_ready_ms_p50",
    "snapshot_vs_rebuild_speedup_p50",
)
with (run_dir / "recovery_results.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=columns)
    writer.writeheader()
    for candidate in candidates:
        prepare = prepare_rows[candidate]
        for cache_state in cache_states:
            source = raw_rows[("source-rebuild", candidate, cache_state)]
            snapshot = raw_rows[("snapshot-load", candidate, cache_state)]
            for key in settings:
                if source[key] != snapshot[key]:
                    raise ValueError(f"paired settings mismatch for {candidate}: {key}")
            if source["expected_checksum"] != snapshot["expected_checksum"]:
                raise ValueError(f"reference checksum mismatch for {candidate}")
            if source["restored_checksum"] != snapshot["restored_checksum"]:
                raise ValueError(f"recovery outputs differ for {candidate}")
            source_p50 = float(source["ready_p50_ms"])
            snapshot_p50 = float(snapshot["ready_p50_ms"])
            if source_p50 <= 0 or snapshot_p50 <= 0:
                raise ValueError("ready-time measurements must be positive")
            for operation in operations:
                row = raw_rows[(operation, candidate, cache_state)]
                rss, elapsed = process_metrics[(operation, candidate, cache_state)]
                writer.writerow({
                    **row,
                    "schema_version": "recovery-v2",
                    "peak_rss_kb": rss,
                    "process_elapsed_seconds": elapsed,
                    "source_bytes": os.path.getsize(run_dir / "edges.bin"),
                    "snapshot_bytes": int(prepare["snapshot_bytes"]),
                    "source_write_ms": prepare["source_write_ms"],
                    "snapshot_write_ms": prepare["snapshot_write_ms"],
                    "source_rebuild_ready_ms_p50": source_p50,
                    "snapshot_load_ready_ms_p50": snapshot_p50,
                    "snapshot_vs_rebuild_speedup_p50": source_p50 / snapshot_p50,
                })

manifest = {
    "manifest_schema_version": "2",
    "recovery_format": "recovery-v2",
    "run_id": os.environ["RECOVERY_TIMESTAMP"],
    "settings": {
        "edges": int(os.environ["RECOVERY_EDGES"]),
        "vertices": int(os.environ["RECOVERY_VERTICES"]),
        "vertex_factor": int(os.environ["RECOVERY_VERTEX_FACTOR"]),
        "distribution": os.environ["RECOVERY_DISTRIBUTION"],
        "seed": int(os.environ["RECOVERY_SEED"]),
        "trials": int(os.environ["RECOVERY_TRIALS"]),
        "warmups": int(os.environ["RECOVERY_WARMUPS"]),
        "process_isolation": "Each candidate/operation/cache-state run uses a new process.",
        "cache_states": {
            "warm-process-restart": "Process restart only; OS page cache remains uncontrolled and may be warm.",
            "best-effort-file-advice": "POSIX_FADV_DONTNEED is issued for the input file before every measured load; the OS may ignore the advisory, so this is not guaranteed cold-cache.",
        },
        "peak_rss_scope": "isolated candidate/operation/cache-state process including final equivalence validation",
    },
    "tools": {
        "benchmark_binary": os.environ["RECOVERY_BENCH_BINARY"],
        "platform": platform.platform(),
    },
    "candidates": list(candidates),
    "consolidated_results": "recovery_results.csv",
    "metrics": {
        "rows": len(candidates) * len(cache_states) * len(operations),
        "equivalence": "PASS",
    },
}
(run_dir / "manifest.json").write_text(
    json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
)
print(f"Recovery manifest: {run_dir / 'manifest.json'}")
PY
