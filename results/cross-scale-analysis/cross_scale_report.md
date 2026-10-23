# Cross-scale benchmark interpretation (5M–20M edges)

Generated from validated Phase 2 matrix CSVs. This is an evidence summary,
not a single-score ranking or a production recommendation.

## Validation and scope

- Scales: 5M, 10M, and 20M edges.
- Each scale contains 27 workload points (3 distributions × 3 hop depths × 3 seeds),
  with three candidates per point: 81 candidate rows per scale, 243 total.
- Protocol matched: 500 queries, 3 timed trials, 1 untimed warmup, vertex factor 8.
- All candidate checksums were cross-checked within workload points; all isolated RSS
  runs exited successfully and supplied a peak RSS value.
- Seed spread is descriptive (population SD across three fixed seeds), not a
  confidence interval. It combines dataset-instance changes and timing noise;
  three seeds are too few for strong inferential claims.

## Balanced matrix means

Each mean below equally weights the 27 workload/seed observations at that scale.
It is a compact overview only; use condition tables for workload-specific comparisons.

| Edges | Candidate | Build p50 (ms) | Fwd scan p50 (ms) | Rev scan p50 (ms) | RCA fwd p50 (us) | RCA rev p50 (us) | Retained estimate (MiB) | Peak RSS mean (MiB) |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 5M | bidirectional_hash | 657.85 | 0.05 | 0.04 | 2027.05 | 1878.73 | 153.10 | 229.04 |
| 5M | csr_csc | 415.54 | 0.03 | 0.04 | 944.44 | 951.69 | 85.83 | 155.88 |
| 5M | forward_hash | 728.46 | 0.05 | n/a | 1783.21 | n/a | 92.38 | 167.73 |
| 10M | bidirectional_hash | 1346.65 | 0.05 | 0.04 | 3520.42 | 3361.71 | 306.51 | 454.77 |
| 10M | csr_csc | 862.45 | 0.04 | 0.04 | 1688.68 | 1714.08 | 171.66 | 308.46 |
| 10M | forward_hash | 1513.96 | 0.04 | n/a | 3044.93 | n/a | 184.42 | 330.48 |
| 20M | bidirectional_hash | 2991.08 | 0.05 | 0.04 | 7505.82 | 6996.64 | 610.51 | 903.53 |
| 20M | csr_csc | 1803.99 | 0.04 | 0.03 | 3725.33 | 3881.03 | 343.32 | 613.63 |
| 20M | forward_hash | 3046.37 | 0.04 | n/a | 6898.12 | n/a | 367.59 | 656.01 |

Maximum isolated peak RSS across all runs was 931,752 KiB (bidirectional_hash, 20000000 edges, hub, 4 hops, seed 20261021).

## Observed patterns (not universal recommendations)

At 20M, CSR+CSC had lower median RCA p50 latency than both hash candidates
in every forward workload-seed point (27/27); reverse RCA was lower than
bidirectional hash in 26/27. Its median paired RCA ratio was 0.442 forward
and 0.506 reverse versus bidirectional hash. This is the most consistent
latency separation in these synthetic runs.

CSR+CSC also had lower one-hop p50 than bidirectional hash in 24/27 forward
and 23/27 reverse points at 20M. Build p50 was lower in 22/27 points versus
bidirectional hash and 19/27 versus forward hash, but p10–p90 includes ratios
above 1: build advantage is less consistent by workload.

At 20M, estimated retained index was 18 B/edge for CSR+CSC, about 32 B/edge
for bidirectional hash, and about 19.3 B/edge for forward hash. Mean isolated
peak RSS was approximately 614 MiB, 904 MiB, and 656 MiB respectively.
This does not include a production memory headroom policy; the maximum observed
isolated run is reported above.

From 5M to 20M, retained-index estimates and peak RSS grew approximately 4×,
close to the 4× edge-count increase. Build p50 grew about 4.2–4.6× in the
balanced means. RCA scale ratios are much less uniform across workloads:
for example, clustered 4-hop rows include per-condition ratios below 0.1 while
hub 3–4-hop rows can approach 5×. Do not treat a single average growth factor
as a traversal scaling law.


### Matched candidate comparison at 20M

Ratios are candidate / comparator across 27 paired workload-seed observations;
below 1 favors the candidate on that metric. The median is accompanied by the
share of paired observations where the candidate was lower.

| Candidate / comparator | Metric | Median ratio | P10–P90 | Lower observations |
|---|---|---:|---:|---:|
| csr_csc / bidirectional_hash | build_p50_ms | 0.512 | 0.460–1.083 | 22/27 (81%) |
| csr_csc / bidirectional_hash | one_hop_forward_p50_ms | 0.464 | 0.207–1.005 | 24/27 (89%) |
| csr_csc / bidirectional_hash | one_hop_reverse_p50_ms | 0.524 | 0.272–1.040 | 23/27 (85%) |
| csr_csc / bidirectional_hash | rca_forward_p50_us | 0.442 | 0.303–0.667 | 27/27 (100%) |
| csr_csc / bidirectional_hash | rca_reverse_p50_us | 0.506 | 0.315–0.744 | 26/27 (96%) |
| csr_csc / bidirectional_hash | retained_index_bytes_est | 0.562 | 0.562–0.562 | 27/27 (100%) |
| csr_csc / bidirectional_hash | peak_rss_kb | 0.681 | 0.674–0.682 | 27/27 (100%) |
| csr_csc / forward_hash | build_p50_ms | 0.453 | 0.422–1.329 | 19/27 (70%) |
| csr_csc / forward_hash | one_hop_forward_p50_ms | 0.487 | 0.338–1.272 | 23/27 (85%) |
| csr_csc / forward_hash | rca_forward_p50_us | 0.496 | 0.384–0.679 | 27/27 (100%) |
| csr_csc / forward_hash | retained_index_bytes_est | 0.953 | 0.899–0.953 | 27/27 (100%) |
| csr_csc / forward_hash | peak_rss_kb | 0.935 | 0.934–0.937 | 27/27 (100%) |

## Scale growth under matched conditions

Each ratio is calculated per matched distribution, hop depth, and seed, then
summarized across all 27 paired observations. P10–P90 shows how much growth
varies across those conditions. A value of 2 means the metric doubled.
Do not interpret ratios for latency as pure algorithmic complexity: fixed graph
generation, allocator, memory pressure, and shared-host noise also contribute.

| Candidate | Scale pair | Build p50 ratio | Forward RCA p50 ratio | Reverse RCA p50 ratio | Retained estimate ratio | Peak RSS ratio |
|---|---|---:|---:|---:|---:|---:|
| bidirectional_hash | 5000000→10000000 | 1.98 (1.60–2.63) | 1.14 (0.09–2.19) | 1.05 (0.13–2.18) | 2.00 (2.00–2.00) | 1.99 (1.98–1.99) |
| bidirectional_hash | 10000000→20000000 | 2.24 (1.80–2.87) | 1.13 (0.75–2.21) | 1.13 (0.88–2.15) | 1.99 (1.99–1.99) | 1.99 (1.99–1.99) |
| bidirectional_hash | 5000000→20000000 | 4.41 (3.61–6.09) | 1.25 (0.09–4.74) | 1.21 (0.13–4.72) | 3.99 (3.99–3.99) | 3.94 (3.94–3.95) |
| csr_csc | 5000000→10000000 | 2.09 (1.78–2.36) | 0.99 (0.10–2.13) | 0.95 (0.12–2.13) | 2.00 (2.00–2.00) | 1.98 (1.98–1.98) |
| csr_csc | 10000000→20000000 | 2.09 (1.84–2.28) | 1.35 (0.90–2.37) | 1.33 (1.06–3.09) | 2.00 (2.00–2.00) | 1.99 (1.99–1.99) |
| csr_csc | 5000000→20000000 | 4.26 (3.69–4.99) | 1.23 (0.10–4.70) | 1.40 (0.13–4.83) | 4.00 (4.00–4.00) | 3.94 (3.93–3.94) |
| forward_hash | 5000000→10000000 | 2.11 (1.54–2.65) | 1.17 (0.07–2.23) | n/a | 2.00 (1.99–2.00) | 1.98 (1.94–1.99) |
| forward_hash | 10000000→20000000 | 2.13 (1.64–2.29) | 1.23 (0.73–2.37) | n/a | 1.99 (1.99–1.99) | 1.99 (1.98–1.99) |
| forward_hash | 5000000→20000000 | 4.28 (3.20–5.15) | 1.33 (0.08–5.02) | n/a | 3.99 (3.96–3.99) | 3.93 (3.84–3.94) |

### Seed-to-seed variability

CV is computed across three fixed seeds within each scale/distribution/hop
condition, then summarized over the 27 conditions; it is descriptive, not
a confidence bound.

| Candidate | Metric | Median CV | P90 CV | Maximum CV |
|---|---|---:|---:|---:|
| bidirectional_hash | build_p50_ms | 6.5% | 11.9% | 27.5% |
| bidirectional_hash | one_hop_forward_p50_ms | 16.0% | 38.6% | 79.5% |
| bidirectional_hash | one_hop_reverse_p50_ms | 17.4% | 28.4% | 32.8% |
| bidirectional_hash | rca_forward_p50_us | 8.8% | 28.9% | 32.3% |
| bidirectional_hash | rca_reverse_p50_us | 8.0% | 31.4% | 46.9% |
| bidirectional_hash | peak_rss_kb | 0.0% | 0.1% | 0.1% |
| csr_csc | build_p50_ms | 3.5% | 8.0% | 13.5% |
| csr_csc | one_hop_forward_p50_ms | 15.0% | 24.8% | 37.9% |
| csr_csc | one_hop_reverse_p50_ms | 14.8% | 24.2% | 35.4% |
| csr_csc | rca_forward_p50_us | 4.2% | 34.6% | 49.8% |
| csr_csc | rca_reverse_p50_us | 10.6% | 31.8% | 33.9% |
| csr_csc | peak_rss_kb | 0.0% | 0.0% | 0.0% |
| forward_hash | build_p50_ms | 3.9% | 10.9% | 20.2% |
| forward_hash | one_hop_forward_p50_ms | 18.0% | 31.4% | 57.1% |
| forward_hash | rca_forward_p50_us | 8.0% | 23.7% | 47.2% |
| forward_hash | peak_rss_kb | 0.1% | 0.1% | 0.1% |

## Interpretation cautions

- Build p50 is one component of build cost. Construction phase metrics and
  build p95/noise are retained in the detailed CSV tables.
- Forward-hash is deliberately forward-only; reverse metrics are not applicable.
- Retained-index size is an estimate. Peak RSS is a separate process-level
  observation that includes temporary and runtime memory.
- Peak RSS values come from isolated candidate runs, while traversal/build rows
  come from the all-candidate benchmark run; compare them as separate measurements.
- Synthetic uniform, hub, and clustered fixtures do not establish production RCA
  graph shape, query mix, memory ceiling, recovery latency, or durable snapshot behavior.
- The same seed value is reused at each scale, but the vertex universe changes;
  these are matched workload settings, not identical edge-prefix graphs. The
  strong RCA ratio spread across distributions/depths should be interpreted
  as workload/instance dependence plus measurement variation, not explained
  as a universal graph-size effect.
- No winner is declared here. Recovery criteria remain unmeasured until Phase 3.
- Build p95 is shown in the detailed tables, but each build distribution has only
  three timed trials; its p95 is effectively the maximum and should not be read
  as a stable tail-latency estimate.

## Source runs

- 5M: `20261022-055327`, 27 workloads, seeds 20261021, 20261022, 20261023.
- 10M: `20261022-061838`, 27 workloads, seeds 20261021, 20261022, 20261023.
- 20M: `20261022-070740`, 27 workloads, seeds 20261021, 20261022, 20261023.
