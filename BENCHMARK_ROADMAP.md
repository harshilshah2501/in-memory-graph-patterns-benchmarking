# Benchmark roadmap (live)

Last updated: 2026-10-21 (Phase 1 kickoff update)

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

Exit criteria:

1. Each candidate reports build/query distributions + retained memory + peak RSS.
2. CSV schema is stable and parseable by downstream analysis scripts.

---

## Phase 2 — Scale and workload matrix execution

| Item | Status | Output |
|---|---|---|
| Scale sweep (5M, 10M, 20M, 30M, 40M) | PLANNED | Per-size CSV runs |
| Distribution sweep (uniform/hub/clustered) | PLANNED | Cross-workload comparison |
| Hop-depth sweep (2/3/4) | PLANNED | RCA depth sensitivity |
| Seed sweep | PLANNED | Repeatability and variance |
| Consolidated matrix summary | PLANNED | Candidate strengths/limits by condition |

Exit criteria:

1. Matrix complete for all planned sizes and workloads.
2. Correctness gates pass in every recorded run.

---

## Phase 3 — Recovery and reconstruction lifecycle

| Item | Status | Output |
|---|---|---|
| Durable edge-source input path | PLANNED | Repeatable rebuild-from-source workflow |
| Snapshot format + load path | PLANNED | Snapshot-to-ready measurement |
| Recovery correctness checks | PLANNED | Build-vs-load graph equivalence |
| Time-to-ready metrics | PLANNED | Cold start and restart timings |

Exit criteria:

1. End-to-end recovery timings available.
2. Recovery correctness validated by checksums/parity gates.

---

## Phase 4 — Decision package

| Item | Status | Output |
|---|---|---|
| Omni-view comparison tables | PLANNED | No single-score reduction |
| Pareto frontier by metric family | PLANNED | Build vs query vs memory tradeoff view |
| Representation recommendation by workload | PLANNED | Conditioned guidance, not universal claim |
| Risks and follow-up experiments | PLANNED | Segmented/delta next-step criteria |

Exit criteria: decision-ready benchmark packet complete.

---

## Immediate next actions (next 3 tasks)

1. Add optional warmup controls so timed runs can separate warm/cold behavior.
2. Add peak RSS fields into consolidated per-run CSV workflow outputs.
3. Add a machine-readable summary manifest for matrix artifacts (schema + run settings + file paths).

---

## Change log

- 2026-10-21: Created live roadmap and seeded status from implemented work.
- 2026-10-21: Completed phase timing + throughput metrics in benchmark output; moved schema hardening to in-progress.
- 2026-10-21: Added isolated-candidate execution and scripted peak RSS capture workflow.
- 2026-10-21: Added stddev/CV/warm-delta noise metrics to benchmark output.
- 2026-10-21: Completed schema hardening with CSV versioning and schema contract documentation.
- 2026-10-21: Added schema validator script and Make target (`benchmark-schema-check`).
- 2026-10-21: Added benchmark matrix runner and consolidated CSV artifact workflow.
