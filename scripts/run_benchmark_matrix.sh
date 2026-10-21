#!/usr/bin/env bash
set -euo pipefail

BENCH_BINARY="${BENCH_BINARY:-./build/graph_storage_bench}"
RSS_SCRIPT="${RSS_SCRIPT:-./scripts/measure_peak_rss.sh}"

EDGES_LIST="${EDGES_LIST:-5000000 10000000 20000000 30000000 40000000}"
DISTRIBUTIONS="${DISTRIBUTIONS:-uniform hub clustered}"
HOPS_LIST="${HOPS_LIST:-2 3}"
TRIALS="${TRIALS:-3}"
QUERIES="${QUERIES:-500}"
SEED="${SEED:-20261021}"
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
  EDGES_LIST     Space-delimited edge counts
  DISTRIBUTIONS  Space-delimited workload distributions
  HOPS_LIST      Space-delimited hop depths
  TRIALS         Trial count passed to benchmark
  QUERIES        Query count passed to benchmark
  SEED           Seed passed to benchmark
  VERTEX_FACTOR  vertices = max(1, edges / VERTEX_FACTOR)
  OUTPUT_ROOT    Artifact output root directory
EOF
  exit 0
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
  echo "trials=$TRIALS"
  echo "queries=$QUERIES"
  echo "seed=$SEED"
  echo "vertex_factor=$VERTEX_FACTOR"
} >"$metadata_txt"

header_written=0
run_counter=0

for edges in $EDGES_LIST; do
  if ! [[ "$edges" =~ ^[0-9]+$ ]]; then
    echo "Invalid edge value in EDGES_LIST: $edges" >&2
    exit 2
  fi
  vertices=$(( edges / VERTEX_FACTOR ))
  if [[ "$vertices" -lt 1 ]]; then
    vertices=1
  fi

  for distribution in $DISTRIBUTIONS; do
    for hops in $HOPS_LIST; do
      run_counter=$((run_counter + 1))
      run_id="run$(printf '%04d' "$run_counter")"

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
        --seed "$SEED" >"$bench_csv"

      "$RSS_SCRIPT" \
        --vertices "$vertices" \
        --edges "$edges" \
        --distribution "$distribution" \
        --hops "$hops" \
        --queries "$QUERIES" \
        --trials 1 \
        --seed "$SEED" >"$rss_csv"

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
      echo "Completed $run_id: edges=$edges vertices=$vertices distribution=$distribution hops=$hops"
    done
  done
done

echo "Matrix run complete."
echo "Consolidated CSV: $summary_csv"
echo "Raw artifacts: $raw_dir"
