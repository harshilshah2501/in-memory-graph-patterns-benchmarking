# In-Memory Graph Representation Benchmark: Final Report

Scope: **5M, 10M and 20M edges**, static graphs, forward and reverse RCA traversal.
30M, 40M and 50M were deferred and are **not validated** (see §9).

## 1. Decision in one page

**Choose `csr_csc`** (compressed sparse row for out-edges plus compressed sparse column for in-edges) when the graph is built in bulk, and then queried in both directions. This is the shape of an RCA index that is built from a snapshot and periodically rebuilt.

| If your situation is... | Choose | Why |
|---|---|---|
| Bulk-built or periodically rebuilt graph, forward and reverse RCA queries, up to about 20M edges | **`csr_csc`** | About 2× faster traversal, about 44% less memory, faster rebuild, and faster build on most graph shapes (§5) |
| You need frequent incremental edge insert or delete that can't be batched | `bidirectional_hash` (or a CSR base plus delta log, not yet benchmarked) | CSR is immutable. Hash maps allow in-place edits. Mutation was **not** benchmarked (§9) |

Only candidates that support **both** forward and reverse traversal are eligible, because RCA needs both. A forward-only hash (`forward_hash`) was also measured, but only as a reference baseline (§2).

Results at the 20M-edge scale (lower is better; full tables in §5). These six are the decision metrics; §3 lists the supporting and diagnostic metrics behind them.

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

## 6. Why `csr_csc` stands out

1. **Contiguous memory.** A vertex's neighbors are one sequential slice. BFS reads sequentially and the hardware prefetcher helps. Hash candidates chase pointers: bucket, node, then a separately allocated vector. At tens of millions of edges these pointers miss the CPU cache, and that is where the ~2× traversal advantage comes from. The gap widens with hop depth (more neighbor lists scanned) and is largest on clustered graphs, where locality helps CSR most.
2. **No per-vertex allocations.** A hash map allocates a node and a vector per vertex (about 2.5M separate allocations at 20M edges, ×2 for bidirectional). CSR uses four flat arrays. This is why memory is lower (18 vs 32 bytes per edge) and why bulk build is faster on uniform and clustered graphs, which are dominated by allocation and insertion cost.
3. **Offsets are cheap.** Both directions cost 8 bytes per edge each plus a small offsets array, giving 18.0 bytes per edge for forward and reverse together.
4. **Where the advantage shrinks:** CSR build does counting-sort passes and sorts each neighbor list for both directions. On hub graphs, a few huge lists make that sort comparatively costly, so its build advantage disappears there. Its relative snapshot speedup is also smaller than bidirectional hash's, because its rebuild is already cheap.
5. **The price:** CSR is **immutable**. Adding or removing an edge means rebuilding or maintaining a separate delta. The benchmark doesn't measure this, because for bulk-built RCA indexes it's usually not the constraint.

## 7. How to choose, decisively

1. **Does the graph change by small, frequent edits that can't be batched?** If yes, CSR alone is the wrong choice. Use `bidirectional_hash` (the only eligible mutable candidate benchmarked), or design a CSR base plus delta log and benchmark it before committing. If edits can be batched into periodic rebuilds, continue.
2. **Otherwise choose `csr_csc`.** It is about 2× faster on deep traversals in both directions, uses about 44% less memory than bidirectional hash, and rebuilds faster on typical graph shapes.
3. **Recovery strategy:** rebuilding from the source costs about 2.8 s at 20M and is already acceptable for many cases. Add a snapshot only if restarts are frequent (break-even is about 4 restarts at 20M) or if source reads are slow in your environment. Expect roughly a 1.4–1.9× improvement, not an order of magnitude.

## 8. Reliability of the evidence

- Every row passes correctness and equivalence checks (243 Phase 2 rows, 324 recovery rows).
- Rankings are consistent across 5M, 10M and 20M and across all three distributions, with the exceptions noted above (hub build, and the smaller relative snapshot speedup).
- The relative gaps are much larger than the seed SD, but there are only 3 seeds, so treat SD as descriptive.

## 9. Limitations (what this does not prove)

| Limitation | Effect |
|---|---|
| Tested up to 20M edges only | 30M to 50M is extrapolated. Rankings are expected to hold because the mechanisms (cache locality, allocation count) strengthen with size, but this is unverified. Memory scales linearly, so 50M CSR+CSC needs roughly 1 GB of index |
| No strict cold-host test | Recovery numbers are warm restart and best-effort file advice only. A real cold start from disk would be slower for every candidate, and is more sensitive to snapshot size |
| Synthetic graphs | No production RCA traces and no edge-type-filtered queries. Real fan-out and locality may differ |
| No update workload | Hash maps' mutability advantage is unmeasured |
| Snapshots not fsynced | No crash-safety or durability claim. The FNV-1a checksum catches accidental corruption only |
| 3 seeds, 3 timed trials | p95 over 3 trials is effectively the maximum |
| Shared 4 GiB, 2-CPU host | Relative results are trustworthy. Absolute times will differ on your hardware |
| Cross-scale comparisons regenerate graphs | Matched in settings, but not identical edge prefixes |

## 10. Next steps to close the gaps

1. 30M and 40M runs (top-of-range check).
2. Strict cold-host recovery test on a dedicated, reboot-able machine.
3. Edge-type-filtered queries and production-trace workloads.
4. A CSR-plus-delta prototype if updates matter.
