# In-Memory Graph Representation Benchmark: Final Report (5M–20M edges)

## 1. Question

Which in-memory graph representation best serves RCA workloads up to roughly 50M edges, judged on:

1. Building / rebuilding the index
2. Forward traversal
3. Reverse (backward) traversal
4. Retained index size (memory)
5. Fast reconstruction / recovery after restart

**Tested scope: 5M, 10M and 20M edges only.** 30M, 40M and 50M were deferred, so conclusions are not validated at the top of the target range.

## 2. Candidates

| Candidate | Structure | Reverse traversal |
|---|---|---|
| `forward_hash` | `unordered_map<vertex, neighbors>` (out-edges only) | None (n/a) |
| `bidirectional_hash` | Two hash adjacency maps (out and in) | Native |
| `csr_csc` | Compressed sparse row (out) plus column (in) arrays | Native |

## 3. Method (summary)

- Synthetic graphs: uniform, hub and clustered distributions; 3 seeds (20261021/22/23); vertex factor 8.
- Traversal: 500 queries per run, 2/3/4-hop forward and reverse RCA expansions, plus one-hop. 3 timed trials and 1 warmup.
- Every run passes correctness gates (candidates must agree on results) and each candidate runs in an isolated process for peak RSS.
- Recovery: durable edge source versus per-candidate binary snapshot, in a fresh process. Each snapshot is checked by checksum and equivalence.
- Host: 4 GiB RAM and 2 CPUs, shared. Results are relative comparisons and not absolute production numbers.
- No composite score is computed.
- Reproduce with the scripts in [README.md](README.md). Data and schemas are in [results/](results/), [BENCHMARK_SCHEMA.md](BENCHMARK_SCHEMA.md) and [RECOVERY_SCHEMA.md](RECOVERY_SCHEMA.md).

## 4. Results at 20M edges

Values are means over 3 seeds and 3 distributions, with hop depths averaged for traversal. Lower is better everywhere.

| Requirement | forward_hash | bidirectional_hash | csr_csc | Standing |
|---|---|---|---|---|
| **Index build** from in-memory edges (p50 ms) | 3046 | 2991 | **1804** | CSR is ~1.7× faster than both hash candidates |
| **Forward traversal** (RCA p50 µs) | 6898 | 7506 | **3725** | CSR is ~1.9–2.0× faster |
| **Reverse traversal** (RCA p50 µs) | n/a | 6997 | **3881** | CSR is ~1.8× faster; forward_hash cannot do it |
| **Retained index** (bytes/edge) | 19.3 | 32.0 | **18.0** | CSR is smallest; bidirectional hash is ~1.8× larger |
| **Query-process peak RSS** (MiB) | 656 | 904 | **614** | CSR is smallest |
| **Rebuild from durable source** (ms) | 3610 | 3756 | **2809** | CSR is fastest |
| **Snapshot load** (ms, warm restart) | **833** | 1679 | 1517 | forward_hash is fastest, but it is forward-only |
| **Snapshot load RSS** (MiB) | 321 | 637 | **347** | forward_hash and CSR are close; bidirectional hash is about 2× |
| **Snapshot size** (MiB) | **134** | 267 | 267 | forward_hash is smallest (one direction only) |
| Snapshot vs rebuild speedup (warm / file-advice) | 4.3× / 3.4× | 2.3× / 1.7× | 1.85× / 1.35× | See caveats |

Results at 5M and 10M have the same ordering (see [results/decision-package/decision_report.md](results/decision-package/decision_report.md)).

### Per-method summary

- **`csr_csc`** is the best on build, forward, reverse, retained size and rebuild. In the Phase 2 matched comparisons at 20M it had lower forward RCA p50 than both hash candidates in 27/27 cases and lower reverse RCA p50 than bidirectional hash in 26/27. It is slower than forward_hash only at snapshot load. It is immutable, so updates mean a rebuild.
- **`bidirectional_hash`** supports forward and reverse, but it is dominated by CSR on build, traversal and memory. It has the largest retained index and snapshot, and its load is about the same as CSR's. Its only potential edge is cheaper incremental updates, which were not benchmarked.
- **`forward_hash`** is competitive on memory and snapshot load, but has no reverse traversal, which RCA requires. It is a baseline, not a viable answer.

## 5. Recovery detail

- Recovery speedups vs rebuilding from source (warm restart, CSR+CSC): 1.5–2.1× across scales and distributions. They are smaller under best-effort cache eviction (about 1.2–1.4×), and hub graphs show different ratios.
- Writing a CSR snapshot pays for itself after about 2 restarts at 5M, 4 at 10M and 4.3 at 20M. These counts depend on the measured write and load times.
- Rebuild from the edge source is already cheap, because CSR construction is linear. A snapshot gives a modest gain, not an order of magnitude, so it is worth adding only if restarts are frequent.

## 6. Recommendation (conditional)

For a **static or periodically rebuilt graph** with forward and reverse RCA traversal up to 20M edges, **choose `csr_csc`**. It meets all three requirements with the best or near-best numbers on every measured axis except snapshot load, where it is about 1.8× slower than forward_hash, which is not viable without reverse traversal.

Choose differently only if:

- You need frequent incremental edge updates. Hash maps support this natively and CSR does not. Consider a segmented design (a CSR base plus a delta log), which has not been benchmarked.
- Restart time dominates and reverse traversal is optional. Then `forward_hash` loads fastest.

## 7. Limitations and open items

| Limitation | Effect |
|---|---|
| Only 5M–20M tested | Scaling to 30M–50M is extrapolated, not measured. A 4 GiB host limits the top scale. |
| No strict cold-host test | Cache states are warm restart and best-effort file advice (`POSIX_FADV_DONTNEED`). True cold-cache needs a dedicated, reboot-able machine. |
| Synthetic graphs | No production RCA traces or edge-type-filtered queries. |
| 3 seeds, 3 trials | Seed standard deviation is descriptive, not a confidence interval. p95 over 3 trials is effectively the max. |
| No update workload | The mutability advantage of hash maps is unmeasured. |
| Snapshots not fsynced | No crash-safety claim. The FNV-1a checksum catches accidental corruption only. |
| Cross-scale comparisons use regenerated graphs | They are matched in settings but are not identical edge prefixes. |

## 8. Suggested next steps

1. Run 30M and 40M when approved, as the top-of-range check.
2. Run a strict cold-host measurement on a dedicated machine.
3. Add edge-type-filtered queries and production-trace workloads.
4. Prototype a CSR-plus-delta design if updates matter.
