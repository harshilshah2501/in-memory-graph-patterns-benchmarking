#!/usr/bin/env bash
set -euo pipefail

BENCH_BINARY="${BENCH_BINARY:-./build/graph_storage_bench}"
RSS_SCRIPT="${RSS_SCRIPT:-./scripts/measure_peak_rss.sh}"
SUMMARY_SCRIPT="${SUMMARY_SCRIPT:-./scripts/summarize_benchmark_matrix.py}"

EDGES_LIST="${EDGES_LIST:-5000000 10000000 20000000 30000000 40000000}"
DISTRIBUTIONS="${DISTRIBUTIONS:-uniform hub clustered}"
HOPS_LIST="${HOPS_LIST:-2 3 4}"
SEEDS_LIST="${SEEDS_LIST:-${SEED:-20261021 20261022 20261023}}"
TRIALS="${TRIALS:-3}"
WARMUPS="${WARMUPS:-0}"
QUERIES="${QUERIES:-500}"
VERTEX_FACTOR="${VERTEX_FACTOR:-8}"
OUTPUT_ROOT="${OUTPUT_ROOT:-./artifacts/benchmark-matrix}"

if [[ "${1:-}" == "--help" ]]; then
  cat <<'EOF'
Usage: run_benchmark_matrix.sh

Runs the benchmark matrix and writes one consolidated CSV containing:
  - benchmark CSV rows per candidate
  - candidate-isolated peak RSS columns merged by representation

Environment variables:
  BENCH_BINARY   Benchmark executable (default ./build/graph_storage_bench)
  RSS_SCRIPT     RSS helper script (default ./scripts/measure_peak_rss.sh)
  SUMMARY_SCRIPT Per-seed summary tool (default ./scripts/summarize_benchmark_matrix.py)
  EDGES_LIST     Space-delimited edge counts
  DISTRIBUTIONS  Space-delimited workload distributions
  HOPS_LIST      Space-delimited hop depths
  SEEDS_LIST     Space-delimited dataset seeds
  TRIALS         Trial count passed to benchmark
  WARMUPS        Untimed warmup iterations per metric (default 0)
  QUERIES        Query count passed to benchmark
  SEED           Backward-compatible single-seed override for SEEDS_LIST
  VERTEX_FACTOR  vertices = max(1, edges / VERTEX_FACTOR)
  OUTPUT_ROOT    Artifact output root directory
EOF
  exit 0
fi
if [[ ! -f "$SUMMARY_SCRIPT" ]]; then
  echo "Summary script not found: $SUMMARY_SCRIPT" >&2
  exit 2
fi

if [[ ! -x "$BENCH_BINARY" ]]; then
  echo "Benchmark binary not found or not executable: $BENCH_BINARY" >&2
  echo "Build it first with: make benchmark" >&2
  exit 2
fi
if [[ ! -x "$RSS_SCRIPT" ]]; then
  echo "RSS script not found or not executable: $RSS_SCRIPT" >&2
  exit 2
fi
if ! [[ "$VERTEX_FACTOR" =~ ^[0-9]+$ ]] || [[ "$VERTEX_FACTOR" -eq 0 ]]; then
  echo "VERTEX_FACTOR must be a positive integer" >&2
  exit 2
fi

timestamp="$(date +%Y%m%d-%H%M%S)"
run_dir="$OUTPUT_ROOT/$timestamp"
raw_dir="$run_dir/raw"
mkdir -p "$raw_dir"
summary_csv="$run_dir/matrix_results.csv"
metadata_txt="$run_dir/run_metadata.txt"

{
  echo "timestamp=$timestamp"
  echo "edges_list=$EDGES_LIST"
  echo "distributions=$DISTRIBUTIONS"
  echo "hops_list=$HOPS_LIST"
  echo "seeds_list=$SEEDS_LIST"
  echo "trials=$TRIALS"
  echo "warmups=$WARMUPS"
  echo "queries=$QUERIES"
  echo "vertex_factor=$VERTEX_FACTOR"
} >"$metadata_txt"

header_written=0
run_counter=0

declare -A seen_seeds=()
for seed in $SEEDS_LIST; do
  if ! [[ "$seed" =~ ^[0-9]+$ ]] || (( 10#$seed > 4294967295 )); then
    echo "Invalid seed in SEEDS_LIST (expected uint32): $seed" >&2
    exit 2
  fi
  if [[ -n "${seen_seeds[$seed]:-}" ]]; then
    echo "Duplicate seed in SEEDS_LIST: $seed" >&2
    exit 2
  fi
  seen_seeds["$seed"]=1
done
if [[ "${#seen_seeds[@]}" -eq 0 ]]; then
  echo "SEEDS_LIST must contain at least one seed" >&2
  exit 2
fi

for edges in $EDGES_LIST; do
  if ! [[ "$edges" =~ ^[0-9]+$ ]]; then
    echo "Invalid edge value in EDGES_LIST: $edges" >&2
    exit 2
  fi
  if (( 10#$edges == 0 || 10#$edges > 40000000 )); then
    echo "Edge count must be between 1 and 40000000: $edges" >&2
    exit 2
  fi
  vertices=$(( 10#$edges / VERTEX_FACTOR ))
  if [[ "$vertices" -lt 1 ]]; then
    vertices=1
  fi

  for distribution in $DISTRIBUTIONS; do
    for hops in $HOPS_LIST; do
      if ! [[ "$hops" =~ ^[0-9]+$ ]] || (( hops == 0 )); then
        echo "Hop depths must be positive integers: $hops" >&2
        exit 2
      fi
      for seed in $SEEDS_LIST; do
        run_counter=$((run_counter + 1))
        run_id="run$(printf '%04d' "$run_counter")_seed${seed}"

        bench_csv="$raw_dir/${run_id}_bench.csv"
        rss_csv="$raw_dir/${run_id}_rss.csv"

        "$BENCH_BINARY" \
          --output csv \
          --vertices "$vertices" \
          --edges "$edges" \
          --distribution "$distribution" \
          --hops "$hops" \
          --queries "$QUERIES" \
          --trials "$TRIALS" \
          --warmups "$WARMUPS" \
          --seed "$seed" >"$bench_csv"

        "$RSS_SCRIPT" \
          --vertices "$vertices" \
          --edges "$edges" \
          --distribution "$distribution" \
          --hops "$hops" \
          --queries "$QUERIES" \
          --trials 1 \
          --warmups "$WARMUPS" \
          --seed "$seed" >"$rss_csv"

        declare -A peak_rss=()
        declare -A rss_elapsed=()
        declare -A rss_exit=()
        while IFS=, read -r candidate peak elapsed exit_code; do
          if [[ "$candidate" == "candidate" ]]; then
            continue
          fi
          peak_rss["$candidate"]="$peak"
          rss_elapsed["$candidate"]="$elapsed"
          rss_exit["$candidate"]="$exit_code"
        done <"$rss_csv"

        bench_header="$(head -n 1 "$bench_csv")"
        if [[ "$header_written" -eq 0 ]]; then
          echo "${bench_header},peak_rss_kb,rss_elapsed_seconds,rss_exit_code,matrix_run_id" >"$summary_csv"
          header_written=1
        fi

        while IFS= read -r row; do
          representation="$(cut -d, -f2 <<<"$row")"
          echo "${row},${peak_rss[$representation]:-},${rss_elapsed[$representation]:-},${rss_exit[$representation]:-},$run_id" >>"$summary_csv"
        done < <(tail -n +2 "$bench_csv")

        unset peak_rss rss_elapsed rss_exit
        echo "Completed $run_id: edges=$edges vertices=$vertices distribution=$distribution hops=$hops seed=$seed"
      done
    done
  done
done

echo "Matrix run complete."
echo "Consolidated CSV: $summary_csv"
echo "Raw artifacts: $raw_dir"
python3 "$SUMMARY_SCRIPT" --input "$summary_csv" --output "$run_dir/seed_summary.csv"
echo "Per-seed summary: $run_dir/seed_summary.csv"

export MATRIX_TIMESTAMP="$timestamp"
export MATRIX_RUN_DIR="$run_dir"
export MATRIX_EDGES_LIST="$EDGES_LIST"
export MATRIX_DISTRIBUTIONS="$DISTRIBUTIONS"
export MATRIX_HOPS_LIST="$HOPS_LIST"
export MATRIX_SEEDS_LIST="$SEEDS_LIST"
export MATRIX_TRIALS="$TRIALS"
export MATRIX_WARMUPS="$WARMUPS"
export MATRIX_QUERIES="$QUERIES"
export MATRIX_VERTEX_FACTOR="$VERTEX_FACTOR"
export MATRIX_RUN_COUNT="$run_counter"
export MATRIX_BENCH_BINARY="$BENCH_BINARY"
export MATRIX_RSS_SCRIPT="$RSS_SCRIPT"
export MATRIX_SUMMARY_SCRIPT="$SUMMARY_SCRIPT"
python3 - <<'PY'
import glob
import json
import os
import pathlib

run_dir = pathlib.Path(os.environ["MATRIX_RUN_DIR"])
raw_dir = run_dir / "raw"
manifest = {
    "manifest_schema_version": "1",
    "benchmark_csv_schema_version": "v2",
    "run_id": os.environ["MATRIX_TIMESTAMP"],
    "settings": {
        "edges": [int(value) for value in os.environ["MATRIX_EDGES_LIST"].split()],
        "distributions": os.environ["MATRIX_DISTRIBUTIONS"].split(),
        "hops": [int(value) for value in os.environ["MATRIX_HOPS_LIST"].split()],
        "seeds": [int(value) for value in os.environ["MATRIX_SEEDS_LIST"].split()],
        "trials": int(os.environ["MATRIX_TRIALS"]),
        "warmups": int(os.environ["MATRIX_WARMUPS"]),
        "queries": int(os.environ["MATRIX_QUERIES"]),
        "vertex_factor": int(os.environ["MATRIX_VERTEX_FACTOR"]),
    },
    "run_count": int(os.environ["MATRIX_RUN_COUNT"]),
    "tools": {
        "benchmark_binary": os.environ["MATRIX_BENCH_BINARY"],
        "rss_script": os.environ["MATRIX_RSS_SCRIPT"],
        "summary_script": os.environ["MATRIX_SUMMARY_SCRIPT"],
    },
    "files": {
        "consolidated_csv": "matrix_results.csv",
        "seed_summary_csv": "seed_summary.csv",
        "metadata": "run_metadata.txt",
        "raw_directory": "raw/",
        "raw_files": sorted(
            str(pathlib.Path(path).relative_to(run_dir))
            for path in glob.glob(str(raw_dir / "*"))
        ),
    },
}
with (run_dir / "manifest.json").open("w", encoding="utf-8") as output:
    json.dump(manifest, output, indent=2)
    output.write("\n")
PY
echo "Manifest: $run_dir/manifest.json"
