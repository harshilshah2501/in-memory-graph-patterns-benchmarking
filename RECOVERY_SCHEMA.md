# Recovery artifact and format contract

Recovery measurements use a separate executable and output contract from the
Phase 1/2 traversal benchmark:

```sh
make benchmark-recovery
```

The runner creates one timestamped run directory under
`artifacts/phase3-recovery/` containing the durable edge source, snapshots for
all three candidates, per-mode raw result CSV/log files, a consolidated
`recovery_results.csv`, and `manifest.json`.

For workload runs collected under one or more roots, validate and consolidate
the child run directories with:

```sh
python3 scripts/summarize_graph_recovery_matrix.py <run-root> [<run-root> ...] \
  --output-root <output-root>
```

It writes `matrix_results.csv` (the validated per-run rows), `seed_summary.csv`
(means and population standard deviations by edge count, distribution,
candidate, and cache state), and `matrix_manifest.json`. With three fixed
seeds, the standard deviation is descriptive only; it is not a confidence
interval.

The completed 5M–20M recovery matrix has 27 workload runs and 324 rows:
three edge scales (5M, 10M, 20M), three distributions, three seeds, three
candidates, two recovery operations, and two cache conditions. The 30M and 40M
scales are outside the current agreed scope.

## Recovery result CSV

The consolidated CSV has 12 rows: source rebuild and snapshot load for each
candidate under two cache conditions. Every candidate/operation/cache-condition
combination runs in its own process. Fields:

- `schema_version`: `recovery-v2`
- `operation`, `candidate`, and workload columns: operation, representation,
  cache condition, edge/vertex counts, distribution, seed, trial count, warmup
  count
- `ready_*_ms`: operation duration distribution in milliseconds. The timed
  operation includes source/snapshot read, integrity validation, index build
  or deserialization, and allocations; it stops when the graph is ready.
- `peak_rss_kb`: `/usr/bin/time -v` maximum RSS for the isolated operation
  process, including its post-timing correctness validation.
- `process_elapsed_seconds`: full isolated process wall time, including
  startup, warmups, timed runs, and correctness validation
- `source_write_ms`, `snapshot_write_ms`: single preparation write durations
  (not a distribution; snapshot time is candidate-specific)
- `source_bytes`, `snapshot_bytes`: persisted artifact sizes
- `*_ready_ms_p50` and `snapshot_vs_rebuild_speedup_p50`: paired p50 summary
  for the same candidate and cache condition
- `expected_checksum`, `restored_checksum`, `equivalence`: graph integrity
  comparison against that candidate's prepared baseline
- `snapshot_vs_rebuild_speedup_p50`: source-rebuild p50 divided by snapshot-load
  p50; greater than 1 means snapshot loading was faster in this run

Warmup iterations are excluded from timing samples. The p95 from three timed
trials is effectively the maximum and is not a stable tail estimate.

## Cache conditions and process restart

- `warm-process-restart`: each operation starts in a new process, but the OS
  page cache is uncontrolled and may already contain the input file. This is a
  restart measurement, not cold-cache.
- `best-effort-file-advice`: immediately before each load, the process opens
  the input file and issues `posix_fadvise(..., POSIX_FADV_DONTNEED)`. Linux and
  the filesystem may ignore or partially honor this advisory. Interpret it as
  an eviction attempt, never as a guaranteed cold-cache result.

The runner intentionally does not write to `/proc/sys/vm/drop_caches`: that
operation affects the whole host and is unsuitable for a shared machine. A
strict cold-host measurement requires a dedicated/rebooted machine and should
be reported separately from these results. Neither condition clears device
controller caches.

## Binary formats

All binary formats use explicit little-endian fixed-width integers, a magic
identifier, a version, count fields, and an FNV-1a 64-bit integrity checksum.
Loaders enforce caller-provided vertex/edge bounds, exact file length, endpoint,
degree/offset validity, and checksum equality before returning data. FNV is an
accidental-corruption check, not a cryptographic authenticity guarantee.

- `edges.bin` (`GEDGES1`): header plus canonical source, target, and edge-type
  records. It is the durable source used for rebuild timing.
- `csr_csc.snapshot` (`GCSRCS1`): graph dimensions, outgoing/incoming offset
  arrays, and the packed outgoing/incoming neighbor arrays. Loading restores
  the index directly without rebuilding from edges.
- `forward_hash.snapshot` (`GFHASH1`): graph dimensions and per-vertex outgoing
  degree/neighbor records. Loading recreates the forward adjacency hash map.
- `bidirectional_hash.snapshot` (`GBHASH1`): graph dimensions and per-vertex
  outgoing and incoming degree/neighbor records. Loading recreates both hash
  maps.

The runner does not currently `fsync` files or the containing directory. These
artifacts are repeatable benchmark inputs, not a claim of power-loss-safe
transactional persistence.

## Interpretation limits

These measurements compare three in-memory representations and their own
snapshot encodings. Source-rebuild reads the same canonical durable edge source
for every candidate. The two cache conditions are deliberately not described
as controlled cold-cache. Process peak RSS is isolated per operation, but
includes setup/runtime overhead and final graph-equivalence validation.
