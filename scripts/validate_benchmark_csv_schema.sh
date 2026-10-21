#!/usr/bin/env bash
set -euo pipefail

BENCH_BINARY="${BENCH_BINARY:-./build/graph_storage_bench}"

EXPECTED_HEADER='schema_version,representation,edges,vertices,distribution,seed,queries,hops,trials,build_min_ms,build_p50_ms,build_p95_ms,build_p99_ms,build_mean_ms,build_stddev_ms,build_cv_percent,build_warm_delta_percent,build_max_ms,one_hop_forward_min_ms,one_hop_forward_p50_ms,one_hop_forward_p95_ms,one_hop_forward_p99_ms,one_hop_forward_mean_ms,one_hop_forward_stddev_ms,one_hop_forward_cv_percent,one_hop_forward_warm_delta_percent,one_hop_forward_max_ms,one_hop_reverse_min_ms,one_hop_reverse_p50_ms,one_hop_reverse_p95_ms,one_hop_reverse_p99_ms,one_hop_reverse_mean_ms,one_hop_reverse_stddev_ms,one_hop_reverse_cv_percent,one_hop_reverse_warm_delta_percent,one_hop_reverse_max_ms,rca_forward_min_us,rca_forward_p50_us,rca_forward_p95_us,rca_forward_p99_us,rca_forward_mean_us,rca_forward_stddev_us,rca_forward_cv_percent,rca_forward_warm_delta_percent,rca_forward_max_us,rca_reverse_min_us,rca_reverse_p50_us,rca_reverse_p95_us,rca_reverse_p99_us,rca_reverse_mean_us,rca_reverse_stddev_us,rca_reverse_cv_percent,rca_reverse_warm_delta_percent,rca_reverse_max_us,build_edges_per_sec_p50,one_hop_forward_qps_mean,one_hop_reverse_qps_mean,rca_forward_qps_mean,rca_reverse_qps_mean,phase_init_maps_p50_ms,phase_degree_count_p50_ms,phase_prefix_sum_p50_ms,phase_reserve_vectors_p50_ms,phase_fill_edges_p50_ms,phase_sort_ranges_p50_ms,retained_index_bytes_est,retained_bytes_per_edge_est,retained_bytes_per_vertex_est,one_hop_forward_checksum,one_hop_reverse_checksum,rca_forward_checksum,rca_reverse_checksum'

if [[ "${1:-}" == "--help" ]]; then
  cat <<'EOF'
Usage: validate_benchmark_csv_schema.sh [--input PATH]

Validates CSV header column order/count for benchmark output schema v1.
By default it runs the benchmark and checks the generated header.
Use --input PATH to validate an existing CSV file.
EOF
  exit 0
fi

header_line=""

if [[ "${1:-}" == "--input" ]]; then
  if [[ $# -ne 2 ]]; then
    echo "Expected exactly one file path after --input" >&2
    exit 2
  fi
  input_path="$2"
  if [[ ! -f "$input_path" ]]; then
    echo "CSV input file not found: $input_path" >&2
    exit 2
  fi
  header_line="$(head -n 1 "$input_path" | tr -d '\r')"
else
  if [[ ! -x "$BENCH_BINARY" ]]; then
    echo "Benchmark binary not found or not executable: $BENCH_BINARY" >&2
    echo "Build it first with: make benchmark" >&2
    exit 2
  fi
  header_line="$("$BENCH_BINARY" --output csv --candidate csr_csc --trials 1 --queries 1 --hops 1 | head -n 1 | tr -d '\r')"
fi

actual_count="$(awk -F, '{print NF}' <<<"$header_line")"
expected_count="$(awk -F, '{print NF}' <<<"$EXPECTED_HEADER")"

if [[ "$actual_count" -ne "$expected_count" ]]; then
  echo "CSV schema mismatch: expected $expected_count columns, got $actual_count" >&2
  exit 1
fi

if [[ "$header_line" != "$EXPECTED_HEADER" ]]; then
  echo "CSV schema mismatch: header order/content differs from schema v1" >&2
  echo "Expected:" >&2
  echo "$EXPECTED_HEADER" >&2
  echo "Actual:" >&2
  echo "$header_line" >&2
  exit 1
fi

echo "CSV schema validation passed (v1, $expected_count columns)."
