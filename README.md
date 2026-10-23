# In-memory graph storage prototype

> **Start here:** [FINAL_REPORT.md](FINAL_REPORT.md) summarizes results and recommendations for cold readers.

This C++17 prototype compares forward-adjacency hashing, bidirectional
adjacency hashing, and bidirectional CSR+CSC for dense integer vertex IDs and
typed directed edges.

## Implemented

- `ForwardHashGraph` is a simple forward-only reconstruction reference.
- `BidirectionalHashGraph` stores outgoing and incoming neighbors in separate
  hash-indexed adjacency vectors.
- `CsrCscGraph` builds outgoing CSR and incoming CSC indexes from one edge list
  using degree counts, prefix sums, and packed neighbor arrays.
- Neighbor ranges are sorted by `(edge type, vertex id)` for deterministic
  iteration. Parallel edges are preserved.
- `SnapshotPublisher` atomically publishes immutable graph snapshots. Readers
  holding an older `shared_ptr` can finish against that consistent snapshot
  while a newer one is published.
- Tests cover both directions, typed and duplicate edges, invalid vertices,
  empty graphs, and snapshot lifetime.
- The benchmark accepts configurable vertex, edge, query, seed, trial, and
  untimed warmup counts, plus uniform, hub-concentrated, and clustered
  synthetic edge distributions.
- Repeated build trials retain no graph between samples, and graph destruction
  is outside the measured build interval. Forward-hash and CSR traversal are
  measured in separate scopes to avoid holding both representations at once.
- The benchmark emits an omni-view per representation:
  - Build distribution: min/p50/p95/p99/mean/max
  - Noise characterization per metric: stddev, CV (%), warm-vs-cold delta (%)
  - Build throughput (edges/s, p50) and query throughput (queries/s, mean)
  - One-hop forward and reverse scan distributions
  - Multi-hop RCA forward and reverse distributions
  - Build phase p50 timing breakdown (candidate-specific phases)
  - One-hop and RCA correctness checksums
  - **Estimated retained index size** in bytes plus B/edge and B/vertex
- The benchmark checks one-hop and RCA checksum parity across representations
  before printing results.
- For isolated runs (for example peak RSS measurement), `--candidate` can run
  one representation at a time: `forward_hash`, `bidirectional_hash`, or
  `csr_csc`.

The benchmark is a reproducible synthetic workload, not a production
performance guarantee. The one-hop forward-hash reference has no reverse
index, so reverse traversal is reported for bidirectional hashing and CSR+CSC.
The synthetic hub and cluster generators are controlled fixtures, not
production degree distributions. Array-capacity reporting does not include
temporary construction buffers or peak resident memory. The current query mix
uses generated seeds and a configurable hop limit; edge-type-filtered queries,
production RCA traces, power-loss-safe transactional persistence, and timed
publication-under-reader behavior are not implemented yet. Atomic snapshot
publication is covered for correctness in tests, not timed as a recovery
mechanism.

## Build and run

```sh
make test
make benchmark
```

Roadmap is tracked in [BENCHMARK_ROADMAP.md](./BENCHMARK_ROADMAP.md).
CSV contract is tracked in [BENCHMARK_SCHEMA.md](./BENCHMARK_SCHEMA.md).

This repository currently enforces a maximum of **40,000,000 edges** for
`graph_storage_bench`, based on measured headroom in this shared 4 GiB
environment.

Scale or change the synthetic workload explicitly, for example:

```sh
./build/graph_storage_bench --vertices 1000000 --edges 8000000 \
  --queries 1000 --trials 7 --warmups 2 --hops 4 --seed 42 --distribution hub
```

Use CSV output for downstream analysis:

```sh
./build/graph_storage_bench --output csv
```

CSV output includes a leading `schema_version` column. Current version: `v2`.
The trailing `warmups` column records how many untimed iterations were excluded
from each reported metric. Warmups default to zero; set `--warmups N` to enable
them. Warmup passes are excluded from the timing samples and warm-delta
calculation.

Measure candidate-isolated peak RSS in one command:

```sh
make benchmark-rss
```

Or pass an explicit workload to the RSS script:

```sh
./scripts/measure_peak_rss.sh --vertices 1000000 --edges 8000000 \
  --queries 500 --trials 1 --hops 2 --distribution uniform
```

Validate benchmark CSV header against the schema contract:

```sh
make benchmark-schema-check
```

Run the Phase 2 matrix and merge per-candidate RSS into one CSV artifact. By
default it covers 5M, 10M, 20M, 30M, and 40M edges; all three distributions;
hop depths 2, 3, and 4; and three seeds. The full grid is 135 workloads, so
start with a smaller selection on constrained machines. Each run directory
contains `matrix_results.csv`, a per-seed `seed_summary.csv`, and a
`manifest.json` describing settings and artifact paths. Set `SEEDS_LIST` to
control seed repeatability and `WARMUPS` for untimed warmups:

```sh
make benchmark-matrix
```

For a small local validation matrix:

```sh
EDGES_LIST="80000 160000" DISTRIBUTIONS="uniform hub" HOPS_LIST="2" \
  SEEDS_LIST="7 8" TRIALS=1 WARMUPS=1 QUERIES=100 make benchmark-matrix
```

The per-seed summary reports the mean and standard deviation across seeds for
each numeric metric while preserving the full trial-level measurements in
`matrix_results.csv`.

Analyze matched 5M, 10M, and 20M matrix runs after all three are complete:

```sh
python3 scripts/analyze_cross_scale.py \
  --run-5m artifacts/phase2-5m/<run-id> \
  --run-10m artifacts/phase2-10m/<run-id> \
  --run-20m artifacts/phase2-20m/<run-id> \
  --output-dir artifacts/cross-scale-analysis
```

The analysis validates manifests, matrix coverage, candidate parity checksums,
and isolated RSS status before producing condition-level means/seed spread,
balanced scale summaries, paired scaling ratios, candidate comparisons, and a
Markdown interpretation report. It does not generate a single weighted score
or recovery recommendation.

## Phase 3 recovery measurements

The recovery workflow persists one versioned binary edge source and a
candidate-specific snapshot for each current representation, then measures in
separate processes:

1. Durable edge-source read plus candidate index rebuild to graph-ready.
2. Direct candidate snapshot load to graph-ready.

The candidates are forward hash, bidirectional hash, and CSR+CSC. Both recovery
paths validate checksums and graph equivalence. Isolated process peak RSS,
artifact sizes, source/snapshot write times, and ready-time distributions are
recorded under two cache conditions:

- `warm-process-restart`: a fresh process, with OS page cache state uncontrolled.
- `best-effort-file-advice`: per-file `POSIX_FADV_DONTNEED` before each load;
  the OS may ignore this hint, so it is not a guaranteed cold-cache run.

```sh
make benchmark-recovery
```

The default is a 5M-edge uniform pilot. Set `EDGES`, `DISTRIBUTION`, `SEED`,
`TRIALS`, `WARMUPS`, and `OUTPUT_ROOT` to configure it. The runner retains
generated artifacts under `artifacts/phase3-recovery/`. It does not globally
drop Linux page caches because that is disruptive on a shared host. A strict
cold-host test requires a dedicated/rebooted machine. Details and binary
format bounds are documented in [RECOVERY_SCHEMA.md](./RECOVERY_SCHEMA.md).

After running multiple workloads, validate and consolidate one or more run
roots. For the full 5M–20M recovery dataset:

```sh
python3 scripts/summarize_graph_recovery_matrix.py \
  artifacts/phase3-recovery-5m-matrix \
  artifacts/phase3-recovery-uniform-scales \
  artifacts/phase3-recovery-10m-20m-distributions \
  --output-root artifacts/phase3-recovery-5m-20m-full-distributions
```

The summarizer checks per-run candidate/operation/cache coverage and checksums,
then writes the combined rows, per-seed descriptive summaries, and a matrix
manifest. This complete dataset covers 5M, 10M, and 20M; uniform, hub, and
clustered workloads; and three seeds. The 30M and 40M scales remain deferred.

## Scope and next experiment

Conventional CSR+CSC is the serving baseline in this prototype. Segmented
reconstruction and mutable deltas are not implemented: they add consistency,
publication, and recovery concerns, and should be evaluated only after a real
localized-update workload is measured. A useful follow-up benchmark would
compare full rebuilds with rebuilding only affected segments, including
mutation-to-visibility latency, edges rewritten, peak memory during
publication, and reader behavior during publication.
