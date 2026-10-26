# In-Memory Graph Representation Benchmark: Final Report

Scope: **5M, 10M, 20M and 30M edges**, static graphs, forward and reverse RCA traversal.
40M and 50M remain untested (see §10).

## 1. Decision in one page

**Choose `csr_csc`** (compressed sparse row for out-edges plus compressed sparse column for in-edges) when the graph is built in bulk, and then queried in both directions. This is the shape of an RCA index that is built from a snapshot and periodically rebuilt.

| If your situation is... | Choose | Why |
|---|---|---|
| Bulk-built or periodically rebuilt graph, forward and reverse RCA queries, up to 30M tested edges | **`csr_csc`** | About 1.6–2× faster traversal, about 44% less memory, faster rebuild, and faster build on most graph shapes (§5) |
| You need frequent incremental edge insert or delete that can't be batched | `bidirectional_hash` (or a CSR base plus delta log, not yet benchmarked) | CSR is immutable. Hash maps allow in-place edits. Mutation was **not** benchmarked (§10) |

Only candidates that support **both** forward and reverse traversal are eligible, because RCA needs both. A forward-only hash (`forward_hash`) was also measured, but only as a reference baseline (§2).

Results at the 20M-edge scale (lower is better; §5.5 adds a 30M extension). These six are the decision metrics; §3 lists the supporting and diagnostic metrics behind them.

| Requirement | bidirectional_hash | csr_csc | csr_csc advantage |
|---|---|---|---|
| Build index (ms) | 2991 | **1804** | 1.7× faster (not on hub graphs, §5.1) |
| Forward traversal (µs) | 7506 | **3725** | 2.0× faster |
| Reverse traversal (µs) | 6997 | **3881** | 1.8× faster |
| Retained index (bytes per edge) | 32.0 | **18.0** | 44% smaller |
| Rebuild from source (ms) | 3756 | **2809** | 1.3× faster |
| Snapshot load (ms, warm) | 1679 | **1517** | 10% faster |

## 2. What the candidates are

All three store the same data: directed edges, each with a 32-bit source, 32-bit target and 16-bit edge type. Vertices are dense integer ids. **Edge convention:** an edge A → B means "A depends on B". *Forward* traversal (out-edges) walks dependencies, which is how candidate root causes are found. *Reverse* traversal (in-edges) finds dependents, which is blast-radius analysis. RCA workflows need both. The benchmark only follows out-edges or in-edges, so timings are the same under the opposite convention; only these labels would swap.

A neighbor entry is `{vertex: u32, type: u16}`, which is 8 bytes with padding.

| Candidate | Layout | Reverse traversal | Status |
|---|---|---|---|
| `bidirectional_hash` | Two maps, `unordered_map<vertex, vector<neighbor>>`, one for out-edges and one for in-edges | Native | **Eligible** |
| `csr_csc` | Out-edges in one flat array plus an offsets array (CSR); in-edges the same way (CSC). Neighbor lists are sorted | Native | **Eligible** |
| `forward_hash` | One such map holding out-edges only | Not supported | Reference baseline, **not eligible** |

`forward_hash` is kept in the data only to show what supporting reverse traversal costs. Going from `forward_hash` to `bidirectional_hash` raises memory from about 19.3 to 32.0 bytes per edge (1.7×), while `csr_csc` provides both directions at 18.0, below even the forward-only hash. It appears in the tables below as a marked baseline column or row. It is excluded from every ranking and from the decision.

In CSR, the neighbors of vertex `v` are the contiguous slice `neighbors[offsets[v] .. offsets[v+1])`. Looking up a vertex is two array reads, and walking its neighbors is a sequential memory scan. In a hash map, the same lookup hashes the key, follows a bucket pointer to a node, and then follows a pointer to a separately allocated vector.

## 3. What each metric means

The **six decision metrics** (build, forward traversal, reverse traversal, retained index, rebuild from source, snapshot load) are the rows of the §1 table. The rest are **supporting** metrics, which give context or cost behind a decision metric, and **diagnostic** metrics, which tell you how far to trust the numbers. They are not compared across candidates on their own.

| Role | Metric | Definition | Why it matters for RCA | How to read it |
|---|---|---|---|---|
| Decision | **Build p50 (ms)** | Median time to construct the in-memory index from an in-memory edge list, over 3 timed trials after 1 warmup | Cost of building or rebuilding the index when data changes | Lower is better. Excludes reading from disk |
| Supporting | **One-hop forward/reverse (ms per batch of 500)** | Median time to read the full neighbor list of each of 500 start vertices, once, with no expansion | Raw adjacency lookup speed, separate from traversal logic | Lower is better. Repeated over the same 500 vertices, so it is cache-friendly and is a best-case lookup cost |
| Decision | **RCA forward (µs)** | Median time for one multi-hop breadth-first expansion from a start vertex to everything it can reach within N hops (2, 3 and 4 hops), following out-edges, with a visited set | "What does this node depend on?" Walks dependencies to find candidate root causes | Lower is better. Grows steeply with hops and with hub vertices |
| Decision | **RCA reverse (µs)** | The same expansion following in-edges | "Who depends on this node?" This is blast-radius (impact) analysis | Lower is better. Only the eligible candidates support it |
| Decision | **Retained index bytes per edge** | Estimated bytes held by the finished index (arrays, hash nodes, vectors), divided by edge count | Memory footprint to keep the index resident | Lower is better. An analytic estimate, not allocator-exact |
| Supporting | **Peak RSS (MiB)** | RSS (Resident Set Size) is the physical RAM a process holds. Peak RSS is the maximum over the process lifetime, measured per candidate in an isolated process with `/usr/bin/time -v`. It is reported in two contexts: **query-process RSS** (§5.3: input edge list, build scratch space, the finished index, and query state) and **snapshot-load RSS** (§5.4: a fresh process that loads a snapshot, with no edge list or build scratch space) | Real memory to provision. Retained bytes is only the steady-state index; peak RSS also covers transient build memory and allocator overhead | Lower is better. Always higher than retained bytes (e.g. `csr_csc` at 20M retains about 360 MB but peaks at 614 MiB in the query process). Compare candidates within the same context only |
| Decision | **Rebuild from source (ms)** | Time, in a fresh process, to read the durable edge file and build the index | Recovery when no snapshot exists | Lower is better |
| Decision | **Snapshot load (ms)** | Time, in a fresh process, to read a pre-built binary index image from disk, validate it with a checksum, and make it queryable | Fast restart and recovery | Lower is better. Excludes the cost of writing the snapshot |
| Supporting | **Snapshot vs rebuild speedup** | Rebuild time divided by snapshot-load time | How much a snapshot helps | Higher is better. Below about 1.5× the saving is modest |
| Supporting | **Snapshot write (ms) and size (MiB)** | Cost and disk size to create the snapshot | One-time cost to enable fast recovery | Feeds the break-even count |
| Supporting | **Break-even restarts** | Snapshot write time divided by (rebuild time minus load time) | How many restarts before the snapshot pays for itself | Lower is better |
| Diagnostic | **Cache state** | `warm-process-restart` is a new process with the OS page cache uncontrolled. `best-effort-file-advice` asks the OS to drop the file's cached pages before loading | Recovery after a restart is highly sensitive to the page cache | **Neither is a strict cold-host guarantee** |
| Diagnostic | **Seed SD** | Population standard deviation of the per-seed means, across 3 seeds | Rough indicator of stability | Descriptive only, not a confidence interval |

### Percentiles: p50, p95 and p99

A percentile is a position in the sorted list of timing samples. **p50** (median): half the samples are faster, so it is the typical cost. **p95** and **p99**: 95% and 99% of samples are faster, so they describe the slow **tail** (1 in 20 and 1 in 100 operations are slower than this). A tail matters because a slow operation is what a user or caller waits on, and RCA traversals vary widely with where they start (a hub vertex fans out far more than an ordinary one).

Which one to use depends on how many samples exist:

| Metric | Samples per run | What is reported |
|---|---|---|
| Build, rebuild, snapshot load, one-hop | 3 (one per timed trial) | **p50 only.** With 3 samples p95 and p99 both equal the maximum, so they add nothing. The median of 3 also resists one noisy trial |
| Forward and reverse traversal | 1,500 (500 queries × 3 trials) | **p50, p95 and p99.** The tail percentiles are real observations (§5.2) |

The mean is not used as a headline for traversal because a few hub-driven slow queries pull it up.

## 4. Method

- **Graphs:** synthetic, with three distributions. *Uniform* picks endpoints at random. *Hub* concentrates edges on a few very high-degree vertices. *Clustered* makes edges mostly local. There are 3 seeds, vertex count is edges ÷ 8, and each scale is regenerated.
- **Queries:** 500 start vertices per run. A traversal is a level-by-level BFS that records each vertex once.
- **Correctness gates:** every candidate must produce identical results (reached counts and checksums) on every query, or the run fails. All reported runs pass. Recovery runs also require the restored index to match its own reference.
- **Isolation:** each candidate runs in its own process so peak RSS is not polluted by another candidate.
- **Volume:** Phase 2 covers 3 scales × 3 distributions × 3 seeds × 3 candidates (243 candidate rows). The recovery matrix has 27 workload runs (3 scales × 3 distributions × 3 seeds) and 324 rows (3 candidates × 2 operations × 2 cache states each).
- **Host:** 4 GiB RAM and 2 CPUs, shared. Treat results as **relative comparisons**, not absolute production numbers.
- **Reproduce:** see [README.md](README.md). Schemas are in [BENCHMARK_SCHEMA.md](BENCHMARK_SCHEMA.md) and [RECOVERY_SCHEMA.md](RECOVERY_SCHEMA.md). Summary data is in [results/](results/), with the joined tables in [results/decision-package/decision_report.md](results/decision-package/decision_report.md).
- **No composite score** is computed. Each requirement is shown separately so you can weight them yourself.

## 5. Results

### 5.1 Build and rebuild (requirement: build and rebuild indexes)

Build p50 in ms at 20M edges, by graph shape:

| Distribution | bidirectional_hash | csr_csc | *(baseline) forward_hash* |
|---|---|---|---|
| Uniform | 3488 | **1621** | *3521* |
| Clustered | 3443 | **1661** | *3681* |
| Hub | **1962** | 2004 | *1555* |
| Mean of all three | 2991 | **1804** | *3046* |

**Reading it:** on uniform and clustered graphs CSR+CSC builds about 2.1× faster than bidirectional hash. **On hub-heavy graphs it does not win**: it is about the same as bidirectional hash (2004 vs 1962 ms). Don't treat "CSR builds fastest" as universal.

Rebuild from the durable source (file read plus build, warm restart), mean over distributions at 20M: bidirectional_hash 3756 ms, **csr_csc 2809 ms** (baseline forward_hash: 3610 ms). The ordering is the same at 5M and 10M.

### 5.2 Forward and reverse traversal (requirements: forward and backward traversal)

RCA p50 at 20M edges, in µs, by hop depth (uniform and hub shown; clustered is similar to uniform):

| Distribution | Hops | Fwd: bidirectional | Fwd: csr_csc | Rev: bidirectional | Rev: csr_csc |
|---|---|---|---|---|---|
| Uniform | 2 | 4.0 | **2.2** | 2.9 | **2.3** |
| Uniform | 4 | 251 | **111** | 271 | **125** |
| Hub | 2 | 769 | **509** | 773 | **510** |
| Hub | 4 | 38,785 | **17,824** | 35,336 | **18,358** |
| Clustered | 4 | 271 | **90** | 239 | **89** |

**Reading it:** CSR+CSC is about 1.3–3× faster than bidirectional hash in the rows shown, in both directions. The smallest gap is uniform 2-hop reverse (2.9 vs 2.3 µs, about 1.3×), where both are tiny. Across 27 matched seed/distribution/hop comparisons at 20M, CSR+CSC had lower forward p50 than bidirectional hash in 27 of 27 and lower reverse p50 in 26 of 27. Hub 4-hop queries are about 100× slower than uniform because the expansion reaches a large fraction of the graph.

**Tail latency at 20M** (µs, mean over 3 seeds; each run's percentile is computed over its 1,500 samples). Format is p50 / p95 / p99:

| Distribution | Hops | Direction | bidirectional_hash | csr_csc |
|---|---|---|---|---|
| Uniform | 2 | Fwd | 4.0 / 7.2 / 9.6 | **2.2 / 3.7 / 4.5** |
| Uniform | 2 | Rev | 2.9 / 5.5 / 7.1 | **2.3 / 4.1 / 5.3** |
| Uniform | 4 | Fwd | 251 / 468 / 597 | **111 / 233 / 449** |
| Uniform | 4 | Rev | 271 / 484 / 630 | **125 / 230 / 295** |
| Clustered | 4 | Fwd | 271 / 535 / 675 | **90 / 166 / 219** |
| Clustered | 4 | Rev | 239 / 442 / 567 | **89 / 160 / 200** |
| Hub | 2 | Fwd | 769 / 1,070 / 1,191 | **509 / 734 / 825** |
| Hub | 2 | Rev | 773 / 1,067 / 1,198 | **510 / 725 / 820** |
| Hub | 4 | Fwd | 38,785 / 58,729 / 65,531 | **17,824 / 26,861 / 30,232** |
| Hub | 4 | Rev | 35,336 / 54,187 / 60,362 | **18,358 / 28,213 / 32,115** |

**Reading it:** CSR+CSC is faster at p50, p95 and p99 in every row, so its advantage is not only a median effect. The smallest tail gap is about 1.3× (uniform 2-hop reverse p99, and uniform 4-hop forward p99: 597 vs 449 µs); the p99 gap is about 2.8–3.1× on clustered 4-hop, 1.9–2.2× on hub 4-hop and about 1.4× on hub 2-hop. In this data the tails are moderate: p99 is about 1.5–4× p50 for both layouts. Percentiles are computed per run and then averaged over seeds, so these are not pooled across seeds.

One-hop lookups (the same 500 start vertices, read once, no expansion) at 20M edges. Time in µs per batch of 500; this is a best-case, cache-warm lookup cost, so treat it as supporting evidence only:

| Distribution | Fwd: bidirectional | Fwd: csr_csc | Rev: bidirectional | Rev: csr_csc |
|---|---|---|---|---|
| Uniform | 10.4 | **6.2** | 11.6 | **6.2** |
| Clustered | 8.7 | **4.6** | 8.2 | **4.4** |
| Hub | 103.5 | **95.7** | **101.6** | 102.8 |

On uniform and clustered graphs CSR+CSC is about 1.7–1.9× faster at the raw lookup, which matches the traversal gap. On hub graphs the gap closes (and reverse is a tie), because lookups are dominated by a few very long neighbor lists that both layouts scan sequentially. This shows the traversal advantage is mostly cache behavior and not only the lookup step.

### 5.3 Retained index and memory (requirement: retained index)

Constant across scales and almost constant across distributions:

| | bidirectional_hash | csr_csc | *(baseline) forward_hash* |
|---|---|---|---|
| Retained bytes per edge | 32.0 | **18.0** | *18.9–20.0* |
| Query-process peak RSS at 20M (MiB, mean) | 904 | **614** | *656* |

**Reading it:** CSR+CSC holds **both directions** in about the space a forward-only hash needs for one. Bidirectional hash needs about 1.8× more.

### 5.4 Recovery and reconstruction (requirement: quick recovery)

Snapshot load p50 at 20M, mean over distributions, warm restart:

| | bidirectional_hash | csr_csc | *(baseline) forward_hash* |
|---|---|---|---|
| Snapshot load (ms) | 1679 | **1517** | *833* |
| Load peak RSS (MiB) | 637 | **347** | *321* |
| Snapshot size (MiB) | 267 | 267 | *134* |
| Speedup vs rebuild, warm | **2.3×** | 1.85× | *4.3×* |
| Speedup vs rebuild, file-advice | **1.7×** | 1.35× | *3.4×* |
| Break-even restarts (warm) | **2.8** | 4.3 | *1.2* |

**Reading it:** in absolute terms CSR+CSC recovers faster than bidirectional hash both ways (rebuild 2809 vs 3756 ms; snapshot load 1517 vs 1679 ms) and loads with about half the memory. Its *relative* snapshot speedup is smaller (1.35–1.9× against 1.7–2.3×) because its rebuild is already cheap: construction is a linear counting-sort pass. The practical meaning is that a CSR+CSC snapshot buys only a modest gain, and its snapshot takes about 4 restarts to repay its write cost, against about 3 for bidirectional hash. Treat the snapshot as an optional optimization for CSR+CSC, not a requirement. The forward-only baseline loads fastest only because it stores one direction (134 MiB), so it isn't a like-for-like comparison.

### 5.5 30M-edge extension

The 30M Phase 2 matrix completed all 81 candidate rows (3 distributions × 3 seeds × 3 candidates); every process exited successfully. The 30M recovery matrix covered 9 workloads and 108 rows (3 candidates × 2 operations × 2 cache states); all checksum/equivalence checks passed.

Means across the three distributions, three hop depths, and three seeds; lower is better. Traversal percentiles are the average of each run's per-query percentile, not a percentile pooled across runs:

| Requirement / diagnostic | bidirectional_hash | csr_csc | csr_csc comparison |
|---|---:|---:|---|
| Build p50 (ms) | 7,101 | **3,647** | 1.95× faster overall |
| Forward traversal p50 (µs) | 12,950 | **7,304** | 1.77× faster |
| Forward traversal p99 (µs) | 23,957 | **14,152** | 1.69× lower on average |
| Reverse traversal p50 (µs) | 12,173 | **7,444** | 1.64× faster |
| Reverse traversal p99 (µs) | 23,474 | **15,357** | 1.53× lower on average |
| Retained index (bytes/edge) | 32.1 | **18.0** | 44% smaller |
| Query-process peak RSS (MiB) | 1,358 | **919** | 32% lower |
| Rebuild from durable source (ms) | 8,685 | **5,879** | 1.48× faster |
| Snapshot load, warm restart (ms) | 3,557 | **2,865** | 1.24× faster |
| Snapshot load, best-effort file advice (ms) | 4,309 | **4,265** | Approximately tied |
| Snapshot break-even restarts (warm) | 1.6 | 3.0 | Snapshot write pays back later |

**Interpretation:** the 30M data supports the CSR+CSC recommendation on the principal RCA criteria: it remains substantially faster on typical forward/reverse traversal, has the same 18-byte-per-edge retained footprint, and uses less peak query RSS than bidirectional hash. It also rebuilds faster. At 30M, however, snapshot-load performance depends on cache condition: CSR+CSC is faster on warm restart and nearly tied under best-effort file advice. Its faster rebuild means the snapshot write takes about 3 restarts to repay, compared with about 1.6 for bidirectional hash.

**Tail caveat:** averaged p99 improves for CSR+CSC, but not in every individual workload. Hub 2-hop and clustered 4-hop include p99 reversals/outliers; with three seeds, treat those individual tail differences as signals to investigate, not as a stable ranking. The 30M results therefore strengthen the p50 and memory conclusion, while making clear that per-workload p99 needs more samples before setting an operational tail-latency guarantee.

Build is distribution-sensitive. At 30M, CSR+CSC averaged about 3.2 s on uniform, 3.9 s on hub, and 3.8 s on clustered; its hub build is about tied with forward-only hash (3.9 s), while bidirectional hash is 5.2 s. Thus the hub build exception at 20M does not overturn the eligible two-way comparison at 30M.

## 6. Why `csr_csc` stands out

1. **Contiguous memory.** A vertex's neighbors are one sequential slice. BFS reads sequentially and the hardware prefetcher helps. Hash candidates chase pointers: bucket, node, then a separately allocated vector. At tens of millions of edges these pointers miss the CPU cache, and that is where the ~2× traversal advantage comes from. The gap widens with hop depth (more neighbor lists scanned) and is largest on clustered graphs, where locality helps CSR most.
2. **No per-vertex allocations.** A hash map allocates a node and a vector per vertex (about 2.5M separate allocations at 20M edges, ×2 for bidirectional). CSR uses four flat arrays. This is why memory is lower (18 vs 32 bytes per edge) and why bulk build is faster on uniform and clustered graphs, which are dominated by allocation and insertion cost.
3. **Offsets are cheap.** Both directions cost 8 bytes per edge each plus a small offsets array, giving 18.0 bytes per edge for forward and reverse together.
4. **Where the advantage shrinks:** CSR build does counting-sort passes and sorts each neighbor list for both directions. On hub graphs, a few huge lists make that sort comparatively costly, so its build advantage disappears there. Its relative snapshot speedup is also smaller than bidirectional hash's, because its rebuild is already cheap.
5. **The price:** CSR is **immutable**. Adding or removing an edge means rebuilding or maintaining a separate delta. The benchmark doesn't measure this, because for bulk-built RCA indexes it's usually not the constraint.

## 7. How to choose, decisively

1. **Does the graph change by small, frequent edits that can't be batched?** If yes, CSR alone is the wrong choice. Use `bidirectional_hash` (the only eligible mutable candidate benchmarked), or design a CSR base plus delta log and benchmark it before committing. If edits can be batched into periodic rebuilds, continue.
2. **Otherwise choose `csr_csc`.** Across tested scales through 30M, it uses about 44% less retained memory than bidirectional hash and delivers faster typical traversal and rebuild. Tail percentiles favor CSR+CSC on average, but the individual 30M p99 results are not uniformly better.
3. **Recovery strategy:** at 30M, rebuild from the source costs about 5.9 s, and warm snapshot load about 2.9 s. Add a snapshot if restarts are frequent or source reads are slow, but account for about 3 warm restarts to repay snapshot writing; under best-effort file advice, load times are nearly tied.

## 8. Reliability of the evidence

- Every 30M Phase 2 process exited successfully; all 108 recovery rows pass equivalence. Prior 5M–20M matrices also passed their correctness/equivalence checks (243 Phase 2 summary rows, 324 recovery rows).
- The broad candidate ordering remains consistent through 30M, with the caveats noted above (distribution-sensitive build and less decisive cache-advised recovery).
- The relative gaps are much larger than the seed SD, but there are only 3 seeds, so treat SD as descriptive.

## 10. Limitations (what this does not prove)

| Limitation | Effect |
|---|---|
| Tested up to 30M edges only | 40M and 50M are untested. Do not extrapolate the recommendation to those sizes without measurement. Memory scales linearly; 50M CSR+CSC would need roughly 900 MB (about 858 MiB) for the retained index estimate alone |
| No strict cold-host test | Recovery numbers are warm restart and best-effort file advice only. A real cold start from disk would be slower for every candidate, and is more sensitive to snapshot size |
| Synthetic graphs | No production RCA traces and no edge-type-filtered queries. Real fan-out and locality may differ |
| No update workload | Hash maps' mutability advantage is unmeasured |
| Snapshots not fsynced | No crash-safety or durability claim. The FNV-1a checksum catches accidental corruption only |
| 3 seeds, 3 timed trials | p95 over 3 trials is effectively the maximum |
| Shared 4 GiB, 2-CPU host | Relative results are trustworthy. Absolute times will differ on your hardware |
| Cross-scale comparisons regenerate graphs | Matched in settings, but not identical edge prefixes |

## 11. Next steps to close the gaps

1. 40M and, if host capacity permits, 50M runs (top-of-range check).
2. Strict cold-host recovery test on a dedicated, reboot-able machine.
3. Edge-type-filtered queries and production-trace workloads.
4. A CSR-plus-delta prototype if updates matter.
