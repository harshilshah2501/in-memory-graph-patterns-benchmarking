# In-Memory Graph Representation Benchmark: Final Report

Scope: **5M, 10M and 20M edges**, static graphs, forward and reverse RCA traversal.
30M, 40M and 50M were deferred and are **not validated** (see §9).

## 1. Decision in one page

**Choose `csr_csc`** (compressed sparse row for out-edges plus compressed sparse column for in-edges) when the graph is built in bulk, and then queried in both directions. This is the shape of an RCA index that is built from a snapshot and periodically rebuilt.

| If your situation is... | Choose | Why |
|---|---|---|
| Bulk-built or periodically rebuilt graph, forward and reverse RCA queries, up to about 20M edges | **`csr_csc`** | About 2× faster traversal, lowest memory, and fastest to build on most graph shapes (§5) |
| You need frequent incremental edge insert or delete, and can't batch it | `bidirectional_hash` (or a CSR base plus delta log, not yet benchmarked) | CSR is immutable. Hash maps allow in-place edits. Mutation was **not** benchmarked (§9) |
| Only forward traversal is needed and fastest restart matters | `forward_hash` | Fastest snapshot load. It cannot do reverse traversal, so it fails the RCA requirement |
| You aren't sure | `csr_csc` | It is the best or near-best on every requirement except snapshot-load speed |

Results at the 20M-edge scale (lower is better; full tables in §5):

| Requirement | forward_hash | bidirectional_hash | csr_csc |
|---|---|---|---|
| Build index (ms) | 3046 | 2991 | **1804** |
| Forward traversal (µs) | 6898 | 7506 | **3725** |
| Reverse traversal (µs) | not supported | 6997 | **3881** |
| Retained index (bytes per edge) | 19.3 | 32.0 | **18.0** |
| Rebuild from source (ms) | 3610 | 3756 | **2809** |
| Snapshot load (ms, warm) | **833** | 1679 | 1517 |

## 2. What the candidates are

All three store the same data: directed edges, each with a 32-bit source, 32-bit target and 16-bit edge type. Vertices are dense integer ids. A neighbor entry is `{vertex: u32, type: u16}`, which is 8 bytes with padding.

| Candidate | Layout | Reverse traversal |
|---|---|---|
| `forward_hash` | `unordered_map<vertex, vector<neighbor>>` holding out-edges only | Not supported |
| `bidirectional_hash` | Two such maps, one for out-edges and one for in-edges | Native |
| `csr_csc` | Out-edges in one flat array plus an offsets array (CSR); in-edges the same way (CSC). Neighbor lists are sorted | Native |

In CSR, the neighbors of vertex `v` are the contiguous slice `neighbors[offsets[v] .. offsets[v+1])`. Looking up a vertex is two array reads, and walking its neighbors is a sequential memory scan. In a hash map, the same lookup hashes the key, follows a bucket pointer to a node, and then follows a pointer to a separately allocated vector.

## 3. What each metric means

| Metric | Definition | Why it matters for RCA | How to read it |
|---|---|---|---|
| **Build p50 (ms)** | Median time to construct the in-memory index from an in-memory edge list, over 3 timed trials after 1 warmup | Cost of building or rebuilding the index when data changes | Lower is better. Excludes reading from disk |
| **One-hop forward/reverse (ms)** | Median time for a batch of single-step neighbor lookups | Raw adjacency lookup speed | Lower is better. Measures structure access cost only |
| **RCA forward (µs)** | Median time for one multi-hop breadth-first expansion from a start vertex to everything it can reach within N hops (2, 3 and 4 hops), following out-edges, with a visited set | "What does this fault impact downstream?" | Lower is better. Grows steeply with hops and with hub vertices |
| **RCA reverse (µs)** | The same expansion following in-edges | "What could have caused this symptom?" Reverse is the core RCA direction | Lower is better. `forward_hash` can't do this |
| **Retained index bytes per edge** | Estimated bytes held by the finished index (arrays, hash nodes, vectors), divided by edge count | Memory footprint to keep the index resident | Lower is better. An analytic estimate, not allocator-exact |
| **Peak RSS (MiB)** | Peak resident memory of the isolated benchmark process, which includes the input edge list and temporary build memory | Real memory you must provision, including during build | Lower is better. Higher than retained bytes because of build scratch space |
| **Rebuild from source (ms)** | Time, in a fresh process, to read the durable edge file and build the index | Recovery when no snapshot exists | Lower is better |
| **Snapshot load (ms)** | Time, in a fresh process, to read a pre-built binary index image from disk, validate it with a checksum, and make it queryable | Fast restart and recovery | Lower is better. Excludes the cost of writing the snapshot |
| **Snapshot vs rebuild speedup** | Rebuild time divided by snapshot-load time | How much a snapshot helps | Higher is better. Below about 1.5× the saving is modest |
| **Snapshot write (ms) and size (MiB)** | Cost and disk size to create the snapshot | One-time cost to enable fast recovery | Feeds the break-even count |
| **Break-even restarts** | Snapshot write time divided by (rebuild time minus load time) | How many restarts before the snapshot pays for itself | Lower is better |
| **Cache state** | `warm-process-restart` is a new process with the OS page cache uncontrolled. `best-effort-file-advice` asks the OS to drop the file's cached pages before loading | Recovery after a restart is highly sensitive to the page cache | **Neither is a strict cold-host guarantee** |
| **Seed SD** | Population standard deviation of the per-seed means, across 3 seeds | Rough indicator of stability | Descriptive only, not a confidence interval |

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

| Distribution | forward_hash | bidirectional_hash | csr_csc |
|---|---|---|---|
| Uniform | 3521 | 3488 | **1621** |
| Clustered | 3681 | 3443 | **1661** |
| Hub | **1555** | 1962 | 2004 |
| Mean of all three | 3046 | 2991 | **1804** |

**Reading it:** on uniform and clustered graphs CSR+CSC builds about 2.1× faster than either hash candidate. **On hub-heavy graphs it does not win**: it is on par with bidirectional hash and slower than forward hash (which only builds one direction). Don't treat "CSR builds fastest" as universal.

Rebuild from the durable source (file read plus build, warm restart), mean over distributions at 20M: forward_hash 3610 ms, bidirectional_hash 3756 ms, **csr_csc 2809 ms**. The ordering is the same at 5M and 10M.

### 5.2 Forward and reverse traversal (requirements: forward and backward traversal)

RCA p50 at 20M edges, in µs, by hop depth (uniform and hub shown; clustered is similar to uniform):

| Distribution | Hops | Fwd: forward_hash | Fwd: bidirectional | Fwd: csr_csc | Rev: bidirectional | Rev: csr_csc |
|---|---|---|---|---|---|---|
| Uniform | 2 | 4.1 | 4.0 | **2.2** | 2.9 | 2.3 |
| Uniform | 4 | 214 | 251 | **111** | 271 | **125** |
| Hub | 2 | 765 | 769 | **509** | 773 | **510** |
| Hub | 4 | 36,021 | 38,785 | **17,824** | 35,336 | **18,358** |
| Clustered | 4 | 205 | 271 | **90** | 239 | **89** |

**Reading it:** CSR+CSC is about 1.8–3× faster than both hash candidates in the rows shown, in both directions. The smallest gap is uniform 2-hop reverse (2.9 vs 2.3 µs, about 1.3×), where both are tiny. Across 27 matched seed/distribution/hop comparisons at 20M, CSR+CSC had lower forward p50 than both hash candidates in 27 of 27, and lower reverse p50 than bidirectional hash in 26 of 27. Hub 4-hop queries are about 100× slower than uniform because the expansion reaches a large fraction of the graph.

### 5.3 Retained index and memory (requirement: retained index)

Constant across scales and almost constant across distributions:

| | forward_hash | bidirectional_hash | csr_csc |
|---|---|---|---|
| Retained bytes per edge | 18.9–20.0 | 32.0 | **18.0** |
| Query-process peak RSS at 20M (MiB, mean) | 656 | 904 | **614** |

**Reading it:** CSR+CSC holds **both directions** in about the space the forward-only hash needs for one. Bidirectional hash needs about 1.8× more.

### 5.4 Recovery and reconstruction (requirement: quick recovery)

Snapshot load p50 at 20M, mean over distributions, warm restart:

| | forward_hash | bidirectional_hash | csr_csc |
|---|---|---|---|
| Snapshot load (ms) | **833** | 1679 | 1517 |
| Load peak RSS (MiB) | 321 | 637 | 347 |
| Snapshot size (MiB) | **134** | 267 | 267 |
| Speedup vs rebuild, warm | 4.3× | 2.3× | 1.85× |
| Speedup vs rebuild, file-advice | 3.4× | 1.7× | 1.35× |
| Break-even restarts (warm) | 1.2 | 2.8 | 4.3 |

**Reading it:** this is the one area where CSR+CSC is not the leader. A snapshot of CSR+CSC makes recovery only about 1.4–1.9× faster than rebuilding from the edge source. Rebuild is already cheap because CSR construction is a linear counting-sort pass. By contrast, `forward_hash` gets the biggest speedup because it has the slowest rebuild relative to its simple load. Still, in absolute terms **CSR+CSC rebuilds faster (2809 ms) than the other hash candidates, and its snapshot load (1517 ms) is about equal to bidirectional hash**. Both directions are needed for RCA, so the like-for-like comparison is CSR+CSC against bidirectional hash.

## 6. Why `csr_csc` stands out

1. **Contiguous memory.** A vertex's neighbors are one sequential slice. BFS reads sequentially and the hardware prefetcher helps. Hash candidates chase pointers: bucket, node, then a separately allocated vector. At tens of millions of edges these pointers miss the CPU cache, and that is where the ~2× traversal advantage comes from. The gap widens with hop depth (more neighbor lists scanned) and is largest on clustered graphs, where locality helps CSR most.
2. **No per-vertex allocations.** A hash map allocates a node and a vector per vertex (about 2.5M separate allocations at 20M edges, ×2 for bidirectional). CSR uses four flat arrays. This is why memory is lower (18 vs 32 bytes per edge) and why bulk build is faster on uniform and clustered graphs, which are dominated by allocation and insertion cost.
3. **Offsets are cheap.** Both directions cost 8 bytes per edge each plus a small offsets array, giving 18.0 bytes per edge for forward and reverse together.
4. **Why it loses on some axes:** CSR build does counting-sort passes and sorts each neighbor list for both directions. On hub graphs, a few huge lists make that sort comparatively costly, so a forward-only hash (one direction, no sort) builds faster. Snapshot load for CSR+CSC reads two directions' worth of data (267 MiB), so it is slower than the one-direction `forward_hash` (134 MiB).
5. **The price:** CSR is **immutable**. Adding or removing an edge means rebuilding or maintaining a separate delta. The benchmark doesn't measure this, because for bulk-built RCA indexes it's usually not the constraint.

## 7. How to choose, decisively

1. **Does the graph change by small, frequent edits that can't be batched?** If yes, CSR alone is the wrong choice. Use `bidirectional_hash`, or design a CSR base plus delta log and benchmark it before committing. If edits can be batched into periodic rebuilds, continue.
2. **Do you need reverse traversal?** If no, `forward_hash` is acceptable and has the fastest recovery. RCA normally needs reverse, so this branch usually doesn't apply.
3. **Otherwise choose `csr_csc`.** It is about 2× faster on deep traversals in both directions, uses about 44% less memory than bidirectional hash, and rebuilds faster on typical graph shapes.
4. **Recovery strategy:** rebuilding from the source costs about 2.8 s at 20M and is already acceptable for many cases. Add a snapshot only if restarts are frequent (break-even is about 4 restarts at 20M) or if source reads are slow in your environment. Expect roughly a 1.4–1.9× improvement, not an order of magnitude.

## 8. Reliability of the evidence

- Every row passes correctness and equivalence checks (243 Phase 2 rows, 324 recovery rows).
- Rankings are consistent across 5M, 10M and 20M and across all three distributions, with the exceptions noted above (hub build, snapshot load).
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
