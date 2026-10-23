# Benchmark roadmap (live)

Last updated: 2026-10-22 (Phase 3 candidate/cache instrumentation)

This is the live execution roadmap for the in-memory graph benchmarking program.
It tracks implementation status, decision gates, and next actions for a full
omni-view comparison of candidate graph representations up to 40M edges in this
environment.

## Status legend

- `DONE`: implemented and validated
- `IN_PROGRESS`: actively being implemented
- `PLANNED`: queued and scoped
- `BLOCKED`: waiting on dependency/input

---

## Phase 0 — Foundation and benchmark hygiene

| Item | Status | Notes |
|---|---|---|
| Baseline candidates wired (forward hash, bidirectional hash, CSR+CSC) | DONE | Implemented in [graph_storage.hpp](./include/graph_storage.hpp) |
| 40M hard edge cap | DONE | Enforced in [graph_storage_bench.cpp](./benchmarks/graph_storage_bench.cpp) |
| Build-trial retention bug removed | DONE | Build timing no longer retains all trial graphs |
| Configurable synthetic distributions | DONE | `uniform`, `hub`, `clustered` |
| Multi-hop forward/reverse RCA metrics | DONE | Bounded traversal latencies and checksum parity |
| Omni-view text + CSV reporting | DONE | `--output text|csv` with distributions and checksums |
| Retained index estimate metric | DONE | Added per representation |

Exit criteria: **met**.

---

## Phase 1 — Full omni-view instrumentation

| Item | Status | Output |
|---|---|---|
| Phase-level build timing breakdown | DONE | Candidate-specific phase p50s in text and CSV output |
| Candidate-isolated peak RSS measurement mode | DONE | `--candidate` selector plus `scripts/measure_peak_rss.sh` workflow |
| Throughput metrics (edges/sec, queries/sec) | DONE | Build and query throughput emitted in text and CSV output |
| Variance/stability metrics (CV, warm/cold delta) | DONE | Stddev/CV/warm-delta emitted for build and traversal distributions |
| Result schema hardening | DONE | CSV version field (`schema_version`) + stable column contract in `BENCHMARK_SCHEMA.md` |
| Optional untimed warmups | DONE | `--warmups` is excluded from samples and recorded in CSV schema v2 |
| Matrix RSS consolidation + JSON manifest | DONE | Candidate RSS fields and run settings/artifact paths in each run directory |

Exit criteria:

1. Each candidate reports build/query distributions + retained memory + peak RSS.
2. CSV schema is stable and parseable by downstream analysis scripts.

---

## Phase 2 — Scale and workload matrix execution

| Item | Status | Output |
|---|---|---|
| Configurable matrix covering scales (5M, 10M, 20M, 30M, 40M) | DONE | `EDGES_LIST` with a 40M guard |
| Distribution sweep (uniform/hub/clustered) | DONE | `DISTRIBUTIONS` |
| Hop-depth sweep (2/3/4) | DONE | `HOPS_LIST` |
| Seed sweep | DONE | `SEEDS_LIST`, defaulting to three distinct seeds |
| Per-seed consolidated summary | DONE | `seed_summary.csv` with mean/stddev across seeds |
| 5M-edge full workload slice | DONE | 27 workloads, 81 candidate rows, 3 seeds, all correctness/RSS runs passed |
| 10M-edge full workload slice | DONE | 27 workloads, 81 candidate rows, 3 seeds, all correctness/RSS runs passed |
| 20M-edge full workload slice | DONE | 27 workloads, 81 candidate rows, 3 seeds; all correctness/RSS runs passed |
| Agreed 5M–20M execution | DONE | All three agreed scale bands complete; 30M and 40M deferred by user |
| Cross-scale result interpretation | DONE | Validated matched comparisons, per-condition seed spread, and scaling ratios generated |

Execution exit criteria for the currently agreed 5M–20M scope:

1. Complete all three agreed scale bands across the planned workloads.
2. Pass correctness gates in every recorded run and validate matched cross-scale comparisons.

---

## Phase 3 — Recovery and reconstruction lifecycle

| Item | Status | Output |
|---|---|---|
| Versioned durable edge-source format + loader | DONE | `edges.bin`, checksummed reader, caller-enforced limits |
| Versioned CSR+CSC snapshot + direct load path | DONE | `csr_csc.snapshot`; loads packed indexes without rebuilding |
| Forward-hash and bidirectional-hash snapshots | DONE | Candidate-native adjacency-map restoration paths |
| Recovery correctness checks | DONE | Source rebuild and loaded snapshot checked against baseline graph checksum |
| Isolated time-to-ready and process peak RSS | DONE | Separate process for each candidate/operation/cache condition |
| Initial 5M CSR+CSC pilot | DONE | Uniform workload; snapshot load p50 2.77× faster, equivalence passed |
| Candidate coverage beyond CSR+CSC | DONE | 5M matrix covers all three candidates and both recovery paths |
| Cache/restart measurement controls | DONE | Fresh-process warm-cache condition + best-effort `POSIX_FADV_DONTNEED` condition; no global cache drop |
| 5M all-candidate/cache matrix | DONE | 3 distributions × 3 seeds × 3 candidates × 2 operations × 2 cache conditions; all 108 rows passed equivalence |
| 10M/20M uniform recovery slice | DONE | 3 seeds per scale; all 72 candidate/operation/cache rows passed equivalence |
| 10M/20M hub and clustered recovery slices | DONE | 3 seeds per distribution/scale; all 144 candidate/operation/cache rows passed equivalence |
| 5M–20M all-distribution recovery matrix | DONE | 3 scales × 3 distributions × 3 seeds × 3 candidates × 2 operations × 2 cache conditions; all 324 rows passed equivalence |
| Strict cold-host measurement | PLANNED | Requires a dedicated/rebooted machine; best-effort file advice is not a cold-cache guarantee |

Phase 3 remains in progress pending strict cold-host measurements and any
additional recovery lifecycle scope. Best-effort file advice is not a guarantee
of cold-cache behavior. The binary file contract and limitations are in
[RECOVERY_SCHEMA.md](./RECOVERY_SCHEMA.md).

Exit criteria:

1. Recovery paths and correctness are measured for the representations in scope.
2. Warm/cold cache state and time-to-ready boundaries are explicit.
3. Recovery workload coverage is sufficient for the agreed scale range.

---

## Phase 4 — Decision package

| Item | Status | Output |
|---|---|---|
| Omni-view comparison tables | DONE (5M–20M) | No single-score reduction |
| Pareto frontier by metric family | DONE (5M–20M) | Build vs query vs memory tradeoff view |
| Representation recommendation by workload | DONE (5M–20M) | Conditioned guidance, not universal claim |
| Risks and follow-up experiments | DONE (5M–20M) | Segmented/delta next-step criteria |

---

## Immediate next actions (next 3 tasks)

1. Review `artifacts/decision-package/decision_report.md` (generated by `scripts/analyze_decision_package.py`).
2. Commit and push.
3. Keep 30M and 40M deferred; reserve genuine cold-host testing for a dedicated machine.

---

## Change log

- 2026-10-21: Created live roadmap and seeded status from implemented work.
- 2026-10-21: Completed phase timing + throughput metrics in benchmark output; moved schema hardening to in-progress.
- 2026-10-21: Added isolated-candidate execution and scripted peak RSS capture workflow.
- 2026-10-21: Added stddev/CV/warm-delta noise metrics to benchmark output.
- 2026-10-21: Completed schema hardening with CSV versioning and schema contract documentation.
- 2026-10-21: Added schema validator script and Make target (`benchmark-schema-check`).
- 2026-10-21: Added benchmark matrix runner and consolidated CSV artifact workflow.
- 2026-10-22: Added untimed `--warmups` controls and recorded the count in CSV schema v2.
- 2026-10-22: Documented the matrix RSS columns and generated a JSON artifact manifest per run.
- 2026-10-22: Added Phase 2 seed sweeps and per-seed mean/stddev summaries; full-scale execution remains pending.
- 2026-10-22: Completed the full 5M-edge slice (3 distributions × 3 hop depths × 3 seeds); all 27 workloads passed, peak isolated RSS was 236,400 KiB, and the run took about 20 minutes.
- 2026-10-22: Completed the full 10M-edge slice with the same 27 workload points; all correctness/RSS runs passed, peak isolated RSS was 469,244 KiB, and the run took about 36 minutes.
- 2026-10-22: Started the full 20M-edge slice as a dedicated background benchmark task; 30M and 40M are deferred by scope.
- 2026-10-22: Completed the full 20M-edge slice (27 workloads, 81 candidate rows); all correctness/RSS runs passed, peak isolated RSS was 931,752 KiB, and the run took about 1h16m. Cross-scale interpretation remains in progress.
- 2026-10-22: Validated all 243 matched 5M/10M/20M candidate rows and generated the cross-scale condition, balanced-scale, seed-variability, candidate-comparison, and scaling-ratio tables plus an interpretation report. No composite score or recovery conclusion was introduced.
- 2026-10-22: Implemented a versioned durable edge source and CSR+CSC snapshot, direct load and source-rebuild modes in isolated processes, graph equivalence checks, ready-time/RSS reporting, and recovery format documentation. Phase 3 remains in progress pending broader candidate and workload coverage.
- 2026-10-22: Added direct-load snapshots for forward and bidirectional hash candidates; added explicit fresh-process warm-cache and best-effort per-file cache-advice modes. All 12 candidate/operation/cache combinations passed the 20K smoke check; representative-scale recovery coverage remains pending.
- 2026-10-22: Completed the matched 5M all-candidate recovery matrix across uniform/hub/clustered distributions and three seeds (108 rows); all equivalence gates passed. Added a reusable validator/consolidator producing raw matrix rows, descriptive per-seed summaries, and a manifest. Warm-process-restart mean snapshot/rebuild p50 ratios by distribution were: uniform 4.24×/2.06×/1.48× (forward/bidirectional/CSR+CSC), hub 2.89×/1.25×/1.83×, clustered 4.48×/1.78×/1.81×. These are descriptive three-seed results; best-effort file advice varies by candidate/workload and is not a cold-cache conclusion.
- 2026-10-22: Completed the approved 10M and 20M uniform recovery slices across three seeds (72 rows); every candidate/operation/cache equivalence gate passed. Warm-process-restart mean snapshot/rebuild p50 ratios at 10M were 4.90×/2.08×/1.71× and at 20M 5.19×/2.43×/1.78× (forward/bidirectional/CSR+CSC). Best-effort file-advice ratios were lower for most comparisons, but remain advisory cache-state observations, not true cold-cache measurements.
- 2026-10-22: Completed hub and clustered recovery coverage at 10M and 20M across three seeds, closing the previously pending distribution gap. The full 5M–20M recovery matrix now covers three scales × three distributions × three seeds × three candidates × two operations × two cache conditions (324 validated rows); all equivalence gates passed.
