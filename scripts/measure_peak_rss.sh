#!/usr/bin/env bash
set -euo pipefail

BENCH_BINARY="${BENCH_BINARY:-./build/graph_storage_bench}"
CANDIDATES=(forward_hash bidirectional_hash csr_csc)

if [[ ! -x "$BENCH_BINARY" ]]; then
  echo "Benchmark binary not found or not executable: $BENCH_BINARY" >&2
  echo "Build it first with: make benchmark" >&2
  exit 2
fi

for arg in "$@"; do
  if [[ "$arg" == "--candidate" ]]; then
    echo "Do not pass --candidate to this script; it runs all candidates separately." >&2
    exit 2
  fi
done

echo "candidate,peak_rss_kb,elapsed_seconds,exit_code"

for candidate in "${CANDIDATES[@]}"; do
  stdout_file="$(mktemp)"
  stderr_file="$(mktemp)"
  set +e
  /usr/bin/time -v "$BENCH_BINARY" --candidate "$candidate" "$@" >"$stdout_file" 2>"$stderr_file"
  exit_code=$?
  set -e

  peak_rss_kb="$(awk -F: '/Maximum resident set size/ {gsub(/^[ \t]+/, "", $2); print $2}' "$stderr_file" | tail -n 1)"
  elapsed="$(awk '
    /Elapsed \(wall clock\) time/ {
      value = $NF
      count = split(value, part, ":")
      if (count == 3) {
        seconds = part[1] * 3600 + part[2] * 60 + part[3]
      } else if (count == 2) {
        seconds = part[1] * 60 + part[2]
      } else {
        seconds = part[1]
      }
      printf "%.2f\n", seconds
    }
  ' "$stderr_file" | tail -n 1)"
  if [[ -z "$peak_rss_kb" ]]; then
    peak_rss_kb=""
  fi
  if [[ -z "$elapsed" ]]; then
    elapsed=""
  fi

  echo "$candidate,$peak_rss_kb,$elapsed,$exit_code"

  if [[ $exit_code -ne 0 ]]; then
    echo "Candidate run failed: $candidate" >&2
    cat "$stderr_file" >&2
    rm -f "$stdout_file" "$stderr_file"
    exit "$exit_code"
  fi

  rm -f "$stdout_file" "$stderr_file"
done
