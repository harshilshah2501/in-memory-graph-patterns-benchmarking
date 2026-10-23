#!/usr/bin/env python3
"""Validate and compare matched Phase 2 graph benchmark scales."""

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path


SCALES = (5_000_000, 10_000_000, 20_000_000)
EXPECTED_CANDIDATES = {"forward_hash", "bidirectional_hash", "csr_csc"}
WORKLOAD_COLUMNS = ("distribution", "hops", "seed")
METRICS = (
    "build_p50_ms",
    "build_p95_ms",
    "one_hop_forward_p50_ms",
    "one_hop_forward_p95_ms",
    "one_hop_reverse_p50_ms",
    "one_hop_reverse_p95_ms",
    "rca_forward_p50_us",
    "rca_forward_p95_us",
    "rca_reverse_p50_us",
    "rca_reverse_p95_us",
    "retained_index_bytes_est",
    "retained_bytes_per_edge_est",
    "retained_bytes_per_vertex_est",
    "peak_rss_kb",
)
TIME_METRICS = {
    "build_p50_ms",
    "build_p95_ms",
    "one_hop_forward_p50_ms",
    "one_hop_forward_p95_ms",
    "one_hop_reverse_p50_ms",
    "one_hop_reverse_p95_ms",
    "rca_forward_p50_us",
    "rca_forward_p95_us",
    "rca_reverse_p50_us",
    "rca_reverse_p95_us",
}
CHECKSUM_COLUMNS = (
    "one_hop_forward_checksum",
    "one_hop_reverse_checksum",
    "rca_forward_checksum",
    "rca_reverse_checksum",
)
COMPARE_COLUMNS = (
    "distribution",
    "hops",
    "seed",
    "queries",
    "trials",
    "warmups",
)


def read_run(run_dir):
    manifest_path = run_dir / "manifest.json"
    csv_path = run_dir / "matrix_results.csv"
    if not manifest_path.is_file() or not csv_path.is_file():
        raise ValueError(f"{run_dir}: manifest.json or matrix_results.csv missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("manifest_schema_version") != "1":
        raise ValueError(f"{manifest_path}: unsupported manifest schema")
    if manifest.get("benchmark_csv_schema_version") != "v2":
        raise ValueError(f"{manifest_path}: expected benchmark schema v2")
    with csv_path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or "matrix_run_id" not in reader.fieldnames:
            raise ValueError(f"{csv_path}: missing expected matrix CSV columns")
        rows = list(reader)
        required = {
            "schema_version", "representation", "edges", "vertices",
            "distribution", "seed", "queries", "hops", "trials", "warmups",
            "peak_rss_kb", "rss_elapsed_seconds", "rss_exit_code",
            "matrix_run_id", *METRICS, *CHECKSUM_COLUMNS,
        }
        if not required.issubset(reader.fieldnames):
            missing = sorted(required - set(reader.fieldnames))
            raise ValueError(f"{csv_path}: missing columns: {missing}")
    if not rows:
        raise ValueError(f"{csv_path}: no data rows")
    if any(None in row or any(value is None for value in row.values()) for row in rows):
        raise ValueError(f"{csv_path}: one or more rows have inconsistent column counts")
    files = manifest.get("files", {})
    require(files.get("consolidated_csv") == "matrix_results.csv",
            f"{manifest_path}: consolidated CSV path does not match")
    for relative_path in files.get("raw_files", []):
        if not (run_dir / relative_path).is_file():
            raise ValueError(f"{manifest_path}: missing raw file {relative_path}")
    if len(files.get("raw_files", [])) != 54:
        raise ValueError(f"{manifest_path}: expected 54 raw files")
    seed_summary_path = run_dir / files.get("seed_summary_csv", "")
    if not seed_summary_path.is_file():
        raise ValueError(f"{manifest_path}: seed summary missing")
    with seed_summary_path.open(newline="", encoding="utf-8") as source:
        seed_summary_rows = list(csv.DictReader(source))
    if len(seed_summary_rows) != 27:
        raise ValueError(f"{seed_summary_path}: expected 27 per-seed summary rows")
    summary_index = {
        (row["representation"], int(row["edges"]), row["distribution"],
         int(row["hops"])): row
        for row in seed_summary_rows
    }
    if len(summary_index) != 27:
        raise ValueError(f"{seed_summary_path}: duplicate summary groups")
    grouped = defaultdict(list)
    for row in rows:
        grouped[(row["representation"], int(row["edges"]), row["distribution"],
                 int(row["hops"]))].append(row)
    for key, group in grouped.items():
        summary = summary_index.get(key)
        if summary is None or int(summary["seed_count"]) != 3:
            raise ValueError(f"{seed_summary_path}: missing or incomplete group {key}")
        if sorted(map(int, summary["seeds"].split())) != [20261021, 20261022, 20261023]:
            raise ValueError(f"{seed_summary_path}: unexpected seeds for {key}")
        for metric in METRICS:
            raw_values = [row.get(metric, "") for row in group]
            raw_values = [float(value) for value in raw_values if value != ""]
            summary_value = summary.get(f"{metric}_mean_across_seeds", "")
            if not raw_values:
                if summary_value != "":
                    raise ValueError(f"{seed_summary_path}: expected empty {metric} for {key}")
                continue
            expected_mean = sum(raw_values) / len(raw_values)
            actual_mean = float(summary_value)
            if not math.isclose(expected_mean, actual_mean, rel_tol=1e-8, abs_tol=1e-10):
                raise ValueError(f"{seed_summary_path}: {metric} does not match source rows for {key}")
    return manifest, rows


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite_number(row, column, required=True):
    raw = row.get(column, "")
    if raw == "":
        if required:
            raise ValueError(f"missing value for required metric {column}")
        return None
    value = float(raw)
    if not math.isfinite(value):
        raise ValueError(f"non-finite value for {column}: {raw}")
    return value


def population_stats(values):
    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / len(values)
    return mean, math.sqrt(variance)


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="", encoding="utf-8") as destination:
        writer = csv.DictWriter(destination, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def validate_runs(run_args):
    all_rows = []
    manifests = []
    seen_scale = set()

    for expected_edges, run_dir in zip(SCALES, run_args):
        manifest, rows = read_run(run_dir)
        settings = manifest.get("settings", {})
        require(settings.get("edges") == [expected_edges],
                f"{run_dir}: expected only {expected_edges} edges")
        require(expected_edges not in seen_scale, f"duplicate scale {expected_edges}")
        seen_scale.add(expected_edges)
        require(manifest.get("run_count") == 27,
                f"{run_dir}: expected 27 workload runs")
        require(settings.get("distributions") == ["uniform", "hub", "clustered"],
                f"{run_dir}: unexpected distribution coverage")
        require(settings.get("hops") == [2, 3, 4],
                f"{run_dir}: unexpected hop coverage")
        require(settings.get("seeds") == [20261021, 20261022, 20261023],
                f"{run_dir}: unexpected seed coverage")
        require(settings.get("queries") == 500 and settings.get("trials") == 3 and
                settings.get("warmups") == 1,
                f"{run_dir}: run settings differ from the completed matrix protocol")
        require(settings.get("vertex_factor") == 8,
                f"{run_dir}: unexpected vertex factor")
        require(len(rows) == 81, f"{run_dir}: expected 81 candidate rows, got {len(rows)}")

        per_workload = defaultdict(dict)
        for row in rows:
            require(row.get("schema_version") == "v2",
                    f"{run_dir}: unsupported row schema")
            require(int(row["edges"]) == expected_edges,
                    f"{run_dir}: row edge count mismatch")
            require(int(row["vertices"]) == expected_edges // 8,
                    f"{run_dir}: row vertex count mismatch")
            candidate = row["representation"]
            require(candidate in EXPECTED_CANDIDATES,
                    f"{run_dir}: unexpected candidate {candidate}")
            workload_key = tuple(row[column] for column in COMPARE_COLUMNS)
            require(candidate not in per_workload[workload_key],
                    f"{run_dir}: duplicate candidate row for {workload_key}")
            per_workload[workload_key][candidate] = row
            require(row.get("rss_exit_code") == "0",
                    f"{run_dir}: RSS failed for {candidate}/{workload_key}")
            finite_number(row, "peak_rss_kb")
            require(int(row["peak_rss_kb"]) > 0,
                    f"{run_dir}: non-positive peak RSS")
            for column in METRICS:
                if column in ("one_hop_reverse_p50_ms", "one_hop_reverse_p95_ms",
                              "rca_reverse_p50_us", "rca_reverse_p95_us"):
                    finite_number(row, column, required=(candidate != "forward_hash"))
                else:
                    finite_number(row, column)
            for checksum in CHECKSUM_COLUMNS:
                finite_number(row, checksum, required=(
                    checksum not in ("one_hop_reverse_checksum", "rca_reverse_checksum")
                    or candidate != "forward_hash"
                ))

        require(len(per_workload) == 27,
                f"{run_dir}: expected 27 unique workload tuples")
        for workload_key, candidates in per_workload.items():
            require(set(candidates) == EXPECTED_CANDIDATES,
                    f"{run_dir}: incomplete candidates for {workload_key}")
            for checksum in ("one_hop_forward_checksum", "rca_forward_checksum"):
                values = {candidates[c][checksum] for c in EXPECTED_CANDIDATES}
                require(len(values) == 1,
                        f"{run_dir}: checksum mismatch for {checksum}/{workload_key}")
            for checksum in ("one_hop_reverse_checksum", "rca_reverse_checksum"):
                values = {
                    candidates[c][checksum]
                    for c in ("bidirectional_hash", "csr_csc")
                }
                require(len(values) == 1,
                        f"{run_dir}: checksum mismatch for {checksum}/{workload_key}")
        manifests.append(manifest)
        all_rows.extend(rows)
    return manifests, all_rows


def make_condition_summaries(rows):
    groups = defaultdict(list)
    for row in rows:
        key = (int(row["edges"]), row["representation"], row["distribution"],
               int(row["hops"]))
        groups[key].append(row)

    output = []
    for key, group in sorted(groups.items()):
        row_out = dict(zip(("edges", "representation", "distribution", "hops"), key))
        seeds = sorted({int(row["seed"]) for row in group})
        require(len(group) == len(seeds) == 3,
                f"incomplete seed group: {key}, rows={len(group)}, seeds={seeds}")
        row_out["seed_count"] = len(seeds)
        row_out["seeds"] = " ".join(map(str, seeds))
        for metric in METRICS:
            values = [finite_number(row, metric, required=not (
                metric in ("one_hop_reverse_p50_ms", "one_hop_reverse_p95_ms",
                           "rca_reverse_p50_us", "rca_reverse_p95_us")
                and row["representation"] == "forward_hash"
            )) for row in group]
            values = [value for value in values if value is not None]
            if not values:
                row_out[f"{metric}_mean"] = ""
                row_out[f"{metric}_sd_across_seeds"] = ""
                row_out[f"{metric}_cv_percent_across_seeds"] = ""
            else:
                mean, stddev = population_stats(values)
                row_out[f"{metric}_mean"] = mean
                row_out[f"{metric}_sd_across_seeds"] = stddev
                row_out[f"{metric}_cv_percent_across_seeds"] = (
                    100 * stddev / mean if mean else 0
                )
        output.append(row_out)
    return output


def make_scale_summary(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[(int(row["edges"]), row["representation"])].append(row)
    output = []
    for (edges, candidate), group in sorted(groups.items()):
        out = {"edges": edges, "representation": candidate,
               "balanced_observations": len(group)}
        for metric in METRICS:
            values = [finite_number(row, metric, required=not (
                metric in ("one_hop_reverse_p50_ms", "one_hop_reverse_p95_ms",
                           "rca_reverse_p50_us", "rca_reverse_p95_us")
                and candidate == "forward_hash"
            )) for row in group]
            values = [value for value in values if value is not None]
            out[f"{metric}_mean"] = sum(values) / len(values) if values else ""
            out[f"{metric}_median"] = sorted(values)[len(values) // 2] if values else ""
            out[f"{metric}_min"] = min(values) if values else ""
            out[f"{metric}_max"] = max(values) if values else ""
        output.append(out)
    return output


def make_scaling_comparisons(rows):
    index = {}
    for row in rows:
        key = (int(row["edges"]), row["representation"],
               row["distribution"], int(row["hops"]), int(row["seed"]))
        index[key] = row

    pairs = ((5_000_000, 10_000_000), (10_000_000, 20_000_000),
             (5_000_000, 20_000_000))
    output = []
    for candidate in sorted(EXPECTED_CANDIDATES):
        for distribution in ("uniform", "hub", "clustered"):
            for hops in (2, 3, 4):
                for lower, upper in pairs:
                    for metric in METRICS:
                        if candidate == "forward_hash" and metric in (
                            "one_hop_reverse_p50_ms", "one_hop_reverse_p95_ms",
                            "rca_reverse_p50_us", "rca_reverse_p95_us",
                        ):
                            continue
                        ratios = []
                        for seed in (20261021, 20261022, 20261023):
                            low = index[(lower, candidate, distribution, hops, seed)]
                            high = index[(upper, candidate, distribution, hops, seed)]
                            low_value = finite_number(low, metric)
                            high_value = finite_number(high, metric)
                            if low_value == 0:
                                raise ValueError(f"zero denominator in {metric} scaling ratio")
                            ratios.append(high_value / low_value)
                        mean, stddev = population_stats(ratios)
                        output.append({
                            "representation": candidate,
                            "distribution": distribution,
                            "hops": hops,
                            "scale_pair": f"{lower}_to_{upper}",
                            "metric": metric,
                            "seed_count": len(ratios),
                            "mean_ratio_across_seeds": mean,
                            "sd_ratio_across_seeds": stddev,
                            "min_ratio_across_seeds": min(ratios),
                            "max_ratio_across_seeds": max(ratios),
                        })
    return output


def percentile(values, percent):
    ordered = sorted(values)
    index = max(0, math.ceil(percent * len(ordered)) - 1)
    return ordered[index]


def make_scaling_overview(rows):
    index = {
        (int(row["edges"]), row["representation"], row["distribution"],
         int(row["hops"]), int(row["seed"])): row
        for row in rows
    }
    output = []
    for candidate in sorted(EXPECTED_CANDIDATES):
        for metric in METRICS:
            if candidate == "forward_hash" and metric in (
                "one_hop_reverse_p50_ms", "one_hop_reverse_p95_ms",
                "rca_reverse_p50_us", "rca_reverse_p95_us",
            ):
                continue
            for lower, upper in ((5_000_000, 10_000_000),
                                 (10_000_000, 20_000_000),
                                 (5_000_000, 20_000_000)):
                ratios = []
                for distribution in ("uniform", "hub", "clustered"):
                    for hops in (2, 3, 4):
                        for seed in (20261021, 20261022, 20261023):
                            low = index[(lower, candidate, distribution, hops, seed)]
                            high = index[(upper, candidate, distribution, hops, seed)]
                            low_value = finite_number(low, metric)
                            high_value = finite_number(high, metric)
                            if low_value <= 0:
                                raise ValueError(f"non-positive denominator in {metric}")
                            ratios.append(high_value / low_value)
                output.append({
                    "representation": candidate,
                    "metric": metric,
                    "scale_pair": f"{lower}_to_{upper}",
                    "matched_observations": len(ratios),
                    "median_ratio": percentile(ratios, 0.5),
                    "p10_ratio": percentile(ratios, 0.1),
                    "p90_ratio": percentile(ratios, 0.9),
                    "min_ratio": min(ratios),
                    "max_ratio": max(ratios),
                })
    return output


def make_candidate_comparisons(rows):
    groups = defaultdict(dict)
    for row in rows:
        key = (int(row["edges"]), row["distribution"], int(row["hops"]),
               int(row["seed"]))
        groups[key][row["representation"]] = row
    output = []
    metrics = (
        "build_p50_ms",
        "one_hop_forward_p50_ms",
        "one_hop_reverse_p50_ms",
        "rca_forward_p50_us",
        "rca_reverse_p50_us",
        "retained_index_bytes_est",
        "peak_rss_kb",
    )
    for edges in SCALES:
        for candidate in sorted(EXPECTED_CANDIDATES):
            comparators = sorted(EXPECTED_CANDIDATES - {candidate})
            for comparator in comparators:
                for metric in metrics:
                    if metric in ("one_hop_reverse_p50_ms", "rca_reverse_p50_us") and (
                        candidate == "forward_hash" or comparator == "forward_hash"
                    ):
                        continue
                    ratios = []
                    for key, candidates in groups.items():
                        if key[0] != edges:
                            continue
                        numerator = finite_number(candidates[candidate], metric)
                        denominator = finite_number(candidates[comparator], metric)
                        if denominator <= 0:
                            raise ValueError(f"non-positive denominator in {metric}")
                        ratios.append(numerator / denominator)
                    require(len(ratios) == 27,
                            f"candidate comparison coverage mismatch for {edges}/{candidate}/{comparator}/{metric}")
                    output.append({
                        "edges": edges,
                        "candidate": candidate,
                        "comparator": comparator,
                        "metric": metric,
                        "matched_observations": len(ratios),
                        "median_ratio": percentile(ratios, 0.5),
                        "p10_ratio": percentile(ratios, 0.1),
                        "p90_ratio": percentile(ratios, 0.9),
                        "candidate_lower_count": sum(ratio < 1 for ratio in ratios),
                        "candidate_equal_count": sum(ratio == 1 for ratio in ratios),
                        "candidate_lower_fraction": sum(ratio < 1 for ratio in ratios) / len(ratios),
                    })
    return output


def make_noise_summary(condition_summary):
    output = []
    for candidate in sorted(EXPECTED_CANDIDATES):
        for metric in ("build_p50_ms", "one_hop_forward_p50_ms",
                       "one_hop_reverse_p50_ms", "rca_forward_p50_us",
                       "rca_reverse_p50_us", "peak_rss_kb"):
            values = [
                float(row[f"{metric}_cv_percent_across_seeds"])
                for row in condition_summary
                if row["representation"] == candidate
                and row[f"{metric}_cv_percent_across_seeds"] != ""
            ]
            if values:
                output.append({
                    "representation": candidate,
                    "metric": metric,
                    "condition_count": len(values),
                    "median_cv_percent": percentile(values, 0.5),
                    "p90_cv_percent": percentile(values, 0.9),
                    "max_cv_percent": max(values),
                })
    return output


def fmt(value, digits=3):
    return f"{value:.{digits}f}"


def render_report(manifests, rows, condition_summary, scale_summary, ratios,
                  growth_overview, candidate_comparisons, noise_summary):
    lines = [
        "# Cross-scale benchmark interpretation (5M–20M edges)",
        "",
        "Generated from validated Phase 2 matrix CSVs. This is an evidence summary,",
        "not a single-score ranking or a production recommendation.",
        "",
        "## Validation and scope",
        "",
        "- Scales: 5M, 10M, and 20M edges.",
        "- Each scale contains 27 workload points (3 distributions × 3 hop depths × 3 seeds),",
        "  with three candidates per point: 81 candidate rows per scale, 243 total.",
        "- Protocol matched: 500 queries, 3 timed trials, 1 untimed warmup, vertex factor 8.",
        "- All candidate checksums were cross-checked within workload points; all isolated RSS",
        "  runs exited successfully and supplied a peak RSS value.",
        "- Seed spread is descriptive (population SD across three fixed seeds), not a",
        "  confidence interval. It combines dataset-instance changes and timing noise;",
        "  three seeds are too few for strong inferential claims.",
        "",
        "## Balanced matrix means",
        "",
        "Each mean below equally weights the 27 workload/seed observations at that scale.",
        "It is a compact overview only; use condition tables for workload-specific comparisons.",
        "",
        "| Edges | Candidate | Build p50 (ms) | Fwd scan p50 (ms) | Rev scan p50 (ms) | RCA fwd p50 (us) | RCA rev p50 (us) | Retained estimate (MiB) | Peak RSS mean (MiB) |",
        "|---:|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in scale_summary:
        def val(metric, multiplier=1, digits=2):
            raw = row[f"{metric}_mean"]
            return "n/a" if raw == "" else fmt(float(raw) * multiplier, digits)
        lines.append(
            f"| {int(row['edges'])/1e6:.0f}M | {row['representation']} | "
            f"{val('build_p50_ms')} | {val('one_hop_forward_p50_ms')} | "
            f"{val('one_hop_reverse_p50_ms')} | {val('rca_forward_p50_us')} | "
            f"{val('rca_reverse_p50_us')} | "
            f"{val('retained_index_bytes_est', 1/(1024*1024))} | "
            f"{val('peak_rss_kb', 1/1024)} |"
        )

    peak_row = max(rows, key=lambda row: float(row["peak_rss_kb"]))
    lines.extend([
        "",
        f"Maximum isolated peak RSS across all runs was "
        f"{int(float(peak_row['peak_rss_kb'])):,} KiB "
        f"({peak_row['representation']}, {peak_row['edges']} edges, "
        f"{peak_row['distribution']}, {peak_row['hops']} hops, "
        f"seed {peak_row['seed']}).",
        "",
        "## Observed patterns (not universal recommendations)",
        "",
        "At 20M, CSR+CSC had lower median RCA p50 latency than both hash candidates",
        "in every forward workload-seed point (27/27); reverse RCA was lower than",
        "bidirectional hash in 26/27. Its median paired RCA ratio was 0.442 forward",
        "and 0.506 reverse versus bidirectional hash. This is the most consistent",
        "latency separation in these synthetic runs.",
        "",
        "CSR+CSC also had lower one-hop p50 than bidirectional hash in 24/27 forward",
        "and 23/27 reverse points at 20M. Build p50 was lower in 22/27 points versus",
        "bidirectional hash and 19/27 versus forward hash, but p10–p90 includes ratios",
        "above 1: build advantage is less consistent by workload.",
        "",
        "At 20M, estimated retained index was 18 B/edge for CSR+CSC, about 32 B/edge",
        "for bidirectional hash, and about 19.3 B/edge for forward hash. Mean isolated",
        "peak RSS was approximately 614 MiB, 904 MiB, and 656 MiB respectively.",
        "This does not include a production memory headroom policy; the maximum observed",
        "isolated run is reported above.",
        "",
        "From 5M to 20M, retained-index estimates and peak RSS grew approximately 4×,",
        "close to the 4× edge-count increase. Build p50 grew about 4.2–4.6× in the",
        "balanced means. RCA scale ratios are much less uniform across workloads:",
        "for example, clustered 4-hop rows include per-condition ratios below 0.1 while",
        "hub 3–4-hop rows can approach 5×. Do not treat a single average growth factor",
        "as a traversal scaling law.",
        "",
    ])

    lines.extend([
        "",
        "### Matched candidate comparison at 20M",
        "",
        "Ratios are candidate / comparator across 27 paired workload-seed observations;",
        "below 1 favors the candidate on that metric. The median is accompanied by the",
        "share of paired observations where the candidate was lower.",
        "",
        "| Candidate / comparator | Metric | Median ratio | P10–P90 | Lower observations |",
        "|---|---|---:|---:|---:|",
    ])
    for row in candidate_comparisons:
        if row["edges"] != 20_000_000:
            continue
        if row["candidate"] != "csr_csc":
            continue
        if row["metric"] not in (
            "build_p50_ms", "one_hop_forward_p50_ms", "one_hop_reverse_p50_ms",
            "rca_forward_p50_us", "rca_reverse_p50_us",
            "retained_index_bytes_est", "peak_rss_kb",
        ):
            continue
        lines.append(
            f"| {row['candidate']} / {row['comparator']} | {row['metric']} | "
            f"{fmt(row['median_ratio'], 3)} | {fmt(row['p10_ratio'], 3)}–"
            f"{fmt(row['p90_ratio'], 3)} | "
            f"{row['candidate_lower_count']}/27 "
            f"({fmt(row['candidate_lower_fraction']*100, 0)}%) |"
        )

    lines.extend([
        "",
        "## Scale growth under matched conditions",
        "",
        "Each ratio is calculated per matched distribution, hop depth, and seed, then",
        "summarized across all 27 paired observations. P10–P90 shows how much growth",
        "varies across those conditions. A value of 2 means the metric doubled.",
        "Do not interpret ratios for latency as pure algorithmic complexity: fixed graph",
        "generation, allocator, memory pressure, and shared-host noise also contribute.",
        "",
        "| Candidate | Scale pair | Build p50 ratio | Forward RCA p50 ratio | Reverse RCA p50 ratio | Retained estimate ratio | Peak RSS ratio |",
        "|---|---|---:|---:|---:|---:|---:|",
    ])
    growth_index = {
        (row["representation"], row["scale_pair"], row["metric"]): row
        for row in growth_overview
    }
    for candidate in sorted(EXPECTED_CANDIDATES):
        for pair in ("5000000_to_10000000", "10000000_to_20000000",
                     "5000000_to_20000000"):
            def ratio(metric):
                row = growth_index.get((candidate, pair, metric))
                if row is None:
                    return "n/a"
                return (f"{fmt(row['median_ratio'], 2)} "
                        f"({fmt(row['p10_ratio'], 2)}–{fmt(row['p90_ratio'], 2)})")
            lines.append(
                f"| {candidate} | {pair.replace('_to_', '→')} | "
                f"{ratio('build_p50_ms')} | {ratio('rca_forward_p50_us')} | "
                f"{ratio('rca_reverse_p50_us')} | "
                f"{ratio('retained_index_bytes_est')} | "
                f"{ratio('peak_rss_kb')} |"
            )

    lines.extend([
        "",
        "### Seed-to-seed variability",
        "",
        "CV is computed across three fixed seeds within each scale/distribution/hop",
        "condition, then summarized over the 27 conditions; it is descriptive, not",
        "a confidence bound.",
        "",
        "| Candidate | Metric | Median CV | P90 CV | Maximum CV |",
        "|---|---|---:|---:|---:|",
    ])
    for row in noise_summary:
        if row["metric"] not in (
            "build_p50_ms", "one_hop_forward_p50_ms", "one_hop_reverse_p50_ms",
            "rca_forward_p50_us", "rca_reverse_p50_us", "peak_rss_kb",
        ):
            continue
        lines.append(
            f"| {row['representation']} | {row['metric']} | "
            f"{fmt(row['median_cv_percent'], 1)}% | "
            f"{fmt(row['p90_cv_percent'], 1)}% | "
            f"{fmt(row['max_cv_percent'], 1)}% |"
        )

    lines.extend([
        "",
        "## Interpretation cautions",
        "",
        "- Build p50 is one component of build cost. Construction phase metrics and",
        "  build p95/noise are retained in the detailed CSV tables.",
        "- Forward-hash is deliberately forward-only; reverse metrics are not applicable.",
        "- Retained-index size is an estimate. Peak RSS is a separate process-level",
        "  observation that includes temporary and runtime memory.",
        "- Peak RSS values come from isolated candidate runs, while traversal/build rows",
        "  come from the all-candidate benchmark run; compare them as separate measurements.",
        "- Synthetic uniform, hub, and clustered fixtures do not establish production RCA",
        "  graph shape, query mix, memory ceiling, recovery latency, or durable snapshot behavior.",
        "- The same seed value is reused at each scale, but the vertex universe changes;",
        "  these are matched workload settings, not identical edge-prefix graphs. The",
        "  strong RCA ratio spread across distributions/depths should be interpreted",
        "  as workload/instance dependence plus measurement variation, not explained",
        "  as a universal graph-size effect.",
        "- No winner is declared here. Recovery criteria remain unmeasured until Phase 3.",
        "- Build p95 is shown in the detailed tables, but each build distribution has only",
        "  three timed trials; its p95 is effectively the maximum and should not be read",
        "  as a stable tail-latency estimate.",
        "",
        "## Source runs",
        "",
    ])
    for scale, manifest in zip(SCALES, manifests):
        lines.append(
            f"- {scale//1_000_000}M: `{manifest['run_id']}`, "
            f"{manifest['run_count']} workloads, seeds "
            f"{', '.join(map(str, manifest['settings']['seeds']))}."
        )
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-5m", type=Path, required=True)
    parser.add_argument("--run-10m", type=Path, required=True)
    parser.add_argument("--run-20m", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    run_dirs = (args.run_5m, args.run_10m, args.run_20m)
    manifests, rows = validate_runs(run_dirs)
    condition_summary = make_condition_summaries(rows)
    scale_summary = make_scale_summary(rows)
    ratios = make_scaling_comparisons(rows)
    growth_overview = make_scaling_overview(rows)
    candidate_comparisons = make_candidate_comparisons(rows)
    noise_summary = make_noise_summary(condition_summary)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    condition_columns = ["edges", "representation", "distribution", "hops",
                         "seed_count", "seeds"]
    for metric in METRICS:
        condition_columns.extend((
            f"{metric}_mean",
            f"{metric}_sd_across_seeds",
            f"{metric}_cv_percent_across_seeds",
        ))
    write_csv(args.output_dir / "condition_summary.csv", condition_columns,
              condition_summary)

    scale_columns = ["edges", "representation", "balanced_observations"]
    for metric in METRICS:
        scale_columns.extend((f"{metric}_mean", f"{metric}_median",
                              f"{metric}_min", f"{metric}_max"))
    write_csv(args.output_dir / "balanced_scale_summary.csv", scale_columns,
              scale_summary)

    ratio_columns = ["representation", "distribution", "hops", "scale_pair",
                     "metric", "seed_count", "mean_ratio_across_seeds",
                     "sd_ratio_across_seeds", "min_ratio_across_seeds",
                     "max_ratio_across_seeds"]
    write_csv(args.output_dir / "matched_scaling_ratios.csv", ratio_columns,
              ratios)
    growth_columns = ["representation", "metric", "scale_pair",
                      "matched_observations", "median_ratio", "p10_ratio",
                      "p90_ratio", "min_ratio", "max_ratio"]
    write_csv(args.output_dir / "scaling_overview.csv", growth_columns,
              growth_overview)
    candidate_columns = [
        "edges", "candidate", "comparator", "metric", "matched_observations",
        "median_ratio", "p10_ratio", "p90_ratio", "candidate_lower_count",
        "candidate_equal_count", "candidate_lower_fraction",
    ]
    write_csv(args.output_dir / "candidate_comparisons.csv", candidate_columns,
              candidate_comparisons)
    noise_columns = [
        "representation", "metric", "condition_count", "median_cv_percent",
        "p90_cv_percent", "max_cv_percent",
    ]
    write_csv(args.output_dir / "seed_variability.csv", noise_columns,
              noise_summary)
    report = render_report(manifests, rows, condition_summary, scale_summary,
                           ratios, growth_overview, candidate_comparisons,
                           noise_summary)
    (args.output_dir / "cross_scale_report.md").write_text(report, encoding="utf-8")
    audit = {
        "analysis_schema_version": "1",
        "scales": list(SCALES),
        "runs": [
            {"run_id": manifest["run_id"], "run_count": manifest["run_count"],
             "artifact_path": str(run_dir)}
            for manifest, run_dir in zip(manifests, run_dirs)
        ],
        "candidate_rows": len(rows),
        "condition_summary_rows": len(condition_summary),
        "balanced_summary_rows": len(scale_summary),
        "scaling_ratio_rows": len(ratios),
        "scaling_overview_rows": len(growth_overview),
        "candidate_comparison_rows": len(candidate_comparisons),
        "validation": "passed",
        "files": [
            "cross_scale_report.md",
            "condition_summary.csv",
            "balanced_scale_summary.csv",
            "matched_scaling_ratios.csv",
            "scaling_overview.csv",
            "candidate_comparisons.csv",
            "seed_variability.csv",
        ],
        "limits": [
            "Three fixed seeds; descriptive variability only.",
            "Synthetic workload fixtures only.",
            "Recovery not measured; no universal recommendation.",
        ],
    }
    (args.output_dir / "analysis_manifest.json").write_text(
        json.dumps(audit, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Cross-scale analysis validated {len(rows)} rows across 3 scales.")
    print(f"Analysis artifacts: {args.output_dir}")


if __name__ == "__main__":
    main()
