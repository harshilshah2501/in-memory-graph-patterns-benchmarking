# Benchmark CSV schema contract

Schema version: **v2**

This document defines the machine-readable output contract for:

```sh
./build/graph_storage_bench --output csv
```

The first CSV column is `schema_version`. Downstream tooling must read this
field first and branch parsing logic by version.

## Compatibility policy

- Each schema version's column order is fixed and stable.
- New fields that would break position-based parsers must create a new schema
  version (`v2`, `v3`, ...).
- Any semantic change to an existing field (units, definition, inclusion
  criteria) must bump schema version.
- Optional or unavailable metrics are emitted as empty fields.

## v2 columns (in order)

1. `schema_version`
2. `representation`
3. `edges`
4. `vertices`
5. `distribution`
6. `seed`
7. `queries`
8. `hops`
9. `trials`
10. `build_min_ms`
11. `build_p50_ms`
12. `build_p95_ms`
13. `build_p99_ms`
14. `build_mean_ms`
15. `build_stddev_ms`
16. `build_cv_percent`
17. `build_warm_delta_percent`
18. `build_max_ms`
19. `one_hop_forward_min_ms`
20. `one_hop_forward_p50_ms`
21. `one_hop_forward_p95_ms`
22. `one_hop_forward_p99_ms`
23. `one_hop_forward_mean_ms`
24. `one_hop_forward_stddev_ms`
25. `one_hop_forward_cv_percent`
26. `one_hop_forward_warm_delta_percent`
27. `one_hop_forward_max_ms`
28. `one_hop_reverse_min_ms`
29. `one_hop_reverse_p50_ms`
30. `one_hop_reverse_p95_ms`
31. `one_hop_reverse_p99_ms`
32. `one_hop_reverse_mean_ms`
33. `one_hop_reverse_stddev_ms`
34. `one_hop_reverse_cv_percent`
35. `one_hop_reverse_warm_delta_percent`
36. `one_hop_reverse_max_ms`
37. `rca_forward_min_us`
38. `rca_forward_p50_us`
39. `rca_forward_p95_us`
40. `rca_forward_p99_us`
41. `rca_forward_mean_us`
42. `rca_forward_stddev_us`
43. `rca_forward_cv_percent`
44. `rca_forward_warm_delta_percent`
45. `rca_forward_max_us`
46. `rca_reverse_min_us`
47. `rca_reverse_p50_us`
48. `rca_reverse_p95_us`
49. `rca_reverse_p99_us`
50. `rca_reverse_mean_us`
51. `rca_reverse_stddev_us`
52. `rca_reverse_cv_percent`
53. `rca_reverse_warm_delta_percent`
54. `rca_reverse_max_us`
55. `build_edges_per_sec_p50`
56. `one_hop_forward_qps_mean`
57. `one_hop_reverse_qps_mean`
58. `rca_forward_qps_mean`
59. `rca_reverse_qps_mean`
60. `phase_init_maps_p50_ms`
61. `phase_degree_count_p50_ms`
62. `phase_prefix_sum_p50_ms`
63. `phase_reserve_vectors_p50_ms`
64. `phase_fill_edges_p50_ms`
65. `phase_sort_ranges_p50_ms`
66. `retained_index_bytes_est`
67. `retained_bytes_per_edge_est`
68. `retained_bytes_per_vertex_est`
69. `one_hop_forward_checksum`
70. `one_hop_reverse_checksum`
71. `rca_forward_checksum`
72. `rca_reverse_checksum`
73. `warmups`

## Units and semantics

- `*_ms`: milliseconds
- `*_us`: microseconds
- `*_percent`: percentage, not fraction
- `*_qps_*`: queries per second
- `retained_*`: estimated steady-state index memory, not process RSS
- `*_checksum`: deterministic aggregate checks used for parity/correctness gates
- `warmups`: number of untimed iterations run before each reported metric

## Missing values

Missing fields are expected for representations that do not support a metric.
For example, `forward_hash` has no reverse one-hop scan, so reverse
one-hop fields are empty.

## Matrix output

`run_benchmark_matrix.sh` preserves the benchmark columns and appends:

- `peak_rss_kb`: candidate-isolated maximum resident set size from `/usr/bin/time`
- `rss_elapsed_seconds`: wall-clock duration of the isolated run, in seconds
- `rss_exit_code`: exit status of that run
- `matrix_run_id`: identifier linking the row to its matrix run

Each matrix run also writes `manifest.json` (manifest schema version `1`) with
the benchmark CSV schema version, workload settings, tool paths, and relative
paths to the consolidated CSV, per-seed summary, metadata, and raw files.
`seed_summary.csv` groups comparable rows by representation and workload
settings and reports the mean and population standard deviation of numeric
metrics across distinct seeds. Checksums and exit codes are excluded from
numeric aggregation.
