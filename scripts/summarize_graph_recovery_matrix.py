#!/usr/bin/env python3
"""Validate and consolidate per-workload graph recovery benchmark runs."""

import argparse
import csv
import json
import pathlib
import statistics


CANDIDATES = {"forward_hash", "bidirectional_hash", "csr_csc"}
CACHE_STATES = {"warm-process-restart", "best-effort-file-advice"}
OPERATIONS = {"source-rebuild", "snapshot-load"}
SETTING_COLUMNS = ("edges", "vertices", "distribution", "seed", "trials", "warmups")


def read_run(run_dir):
    manifest_path = run_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("recovery_format") != "recovery-v2":
        raise ValueError(f"unsupported recovery manifest: {manifest_path}")
    if manifest.get("metrics", {}).get("equivalence") != "PASS":
        raise ValueError(f"manifest equivalence failed: {manifest_path}")
    expected_rows = len(CANDIDATES) * len(CACHE_STATES) * len(OPERATIONS)
    if manifest.get("metrics", {}).get("rows") != expected_rows:
        raise ValueError(f"manifest row count mismatch: {manifest_path}")
    with (run_dir / "recovery_results.csv").open(
        newline="", encoding="utf-8"
    ) as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != expected_rows:
        raise ValueError(f"unexpected recovery row count in {run_dir}")

    settings = manifest["settings"]
    by_identity = {}
    for row in rows:
        if row["schema_version"] != "recovery-v2":
            raise ValueError(f"unsupported row schema in {run_dir}")
        if row["equivalence"] != "PASS":
            raise ValueError(f"equivalence failed in {run_dir}")
        if row["candidate"] not in CANDIDATES:
            raise ValueError(f"unexpected candidate in {run_dir}")
        if row["cache_state"] not in CACHE_STATES:
            raise ValueError(f"unexpected cache state in {run_dir}")
        if row["operation"] not in OPERATIONS:
            raise ValueError(f"unexpected operation in {run_dir}")
        identity = (row["candidate"], row["cache_state"], row["operation"])
        if identity in by_identity:
            raise ValueError(f"duplicate recovery row {identity} in {run_dir}")
        by_identity[identity] = row
        for key in SETTING_COLUMNS:
            if str(settings[key]) != row[key]:
                raise ValueError(f"manifest/row setting mismatch for {key} in {run_dir}")

    expected_identities = {
        (candidate, cache_state, operation)
        for candidate in CANDIDATES
        for cache_state in CACHE_STATES
        for operation in OPERATIONS
    }
    if set(by_identity) != expected_identities:
        raise ValueError(f"incomplete candidate/cache/operation coverage in {run_dir}")
    for candidate in CANDIDATES:
        for cache_state in CACHE_STATES:
            source = by_identity[(candidate, cache_state, "source-rebuild")]
            snapshot = by_identity[(candidate, cache_state, "snapshot-load")]
            if source["expected_checksum"] != snapshot["expected_checksum"]:
                raise ValueError(f"paired expected checksum mismatch in {run_dir}")
            if source["restored_checksum"] != snapshot["restored_checksum"]:
                raise ValueError(f"paired restored checksum mismatch in {run_dir}")
            if source["expected_checksum"] != source["restored_checksum"]:
                raise ValueError(f"source rebuild checksum mismatch in {run_dir}")
    for row in rows:
        row["run_id"] = run_dir.name
    return manifest, rows


def mean_and_population_sd(values):
    return statistics.mean(values), statistics.pstdev(values)


def summarize(rows):
    by_seed = {}
    for row in rows:
        identity = (
            row["edges"],
            row["distribution"],
            row["candidate"],
            row["cache_state"],
            row["seed"],
        )
        by_seed.setdefault(identity, {})[row["operation"]] = row

    groups = {}
    for (edges, distribution, candidate, cache_state, seed), pair in by_seed.items():
        if set(pair) != OPERATIONS:
            raise ValueError(f"incomplete operation pair for seed {seed}")
        source = pair["source-rebuild"]
        snapshot = pair["snapshot-load"]
        rebuild_p50 = float(source["ready_p50_ms"])
        snapshot_p50 = float(snapshot["ready_p50_ms"])
        if rebuild_p50 <= 0 or snapshot_p50 <= 0:
            raise ValueError("ready-time measurements must be positive")
        groups.setdefault((edges, distribution, candidate, cache_state), []).append({
            "edges": int(edges),
            "seed": int(seed),
            "rebuild_p50": rebuild_p50,
            "snapshot_p50": snapshot_p50,
            "speedup": rebuild_p50 / snapshot_p50,
            "rebuild_rss": int(source["peak_rss_kb"]),
            "snapshot_rss": int(snapshot["peak_rss_kb"]),
            "source_bytes": int(source["source_bytes"]),
            "snapshot_bytes": int(source["snapshot_bytes"]),
        })

    output = []
    for (edges, distribution, candidate, cache_state), observations in sorted(groups.items()):
        def stats(key):
            return mean_and_population_sd([item[key] for item in observations])

        rebuild_mean, rebuild_sd = stats("rebuild_p50")
        snapshot_mean, snapshot_sd = stats("snapshot_p50")
        speedup_mean, speedup_sd = stats("speedup")
        rebuild_rss_mean, rebuild_rss_sd = stats("rebuild_rss")
        snapshot_rss_mean, snapshot_rss_sd = stats("snapshot_rss")
        output.append({
            "edges": edges,
            "distribution": distribution,
            "candidate": candidate,
            "cache_state": cache_state,
            "seed_count": len(observations),
            "source_rebuild_p50_mean_ms": f"{rebuild_mean:.6f}",
            "source_rebuild_p50_population_sd_ms": f"{rebuild_sd:.6f}",
            "snapshot_load_p50_mean_ms": f"{snapshot_mean:.6f}",
            "snapshot_load_p50_population_sd_ms": f"{snapshot_sd:.6f}",
            "snapshot_vs_rebuild_speedup_mean": f"{speedup_mean:.6f}",
            "snapshot_vs_rebuild_speedup_population_sd": f"{speedup_sd:.6f}",
            "source_rebuild_peak_rss_mean_kb": f"{rebuild_rss_mean:.2f}",
            "source_rebuild_peak_rss_population_sd_kb": f"{rebuild_rss_sd:.2f}",
            "snapshot_load_peak_rss_mean_kb": f"{snapshot_rss_mean:.2f}",
            "snapshot_load_peak_rss_population_sd_kb": f"{snapshot_rss_sd:.2f}",
            "source_bytes": observations[0]["source_bytes"],
            "snapshot_bytes": observations[0]["snapshot_bytes"],
        })
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "run_roots",
        nargs="+",
        type=pathlib.Path,
        help="One or more roots containing timestamped recovery run directories",
    )
    parser.add_argument(
        "--output-root",
        type=pathlib.Path,
        help="Destination for consolidated CSVs and manifest (default: first run root)",
    )
    args = parser.parse_args()
    for run_root in args.run_roots:
        if not run_root.is_dir():
            raise ValueError(f"run root does not exist: {run_root}")
    output_root = args.output_root or args.run_roots[0]
    output_root.mkdir(parents=True, exist_ok=True)
    run_dirs = sorted({
        path.parent
        for run_root in args.run_roots
        for path in run_root.glob("*/manifest.json")
    })
    if not run_dirs:
        raise ValueError(f"no recovery runs found under {args.run_roots}")
    rows = []
    run_ids = []
    workload_keys = set()
    for run_dir in run_dirs:
        manifest, run_rows = read_run(run_dir)
        settings = manifest["settings"]
        key = (settings["edges"], settings["distribution"], settings["seed"])
        if key in workload_keys:
            raise ValueError(f"duplicate workload settings: {key}")
        workload_keys.add(key)
        rows.extend(run_rows)
        run_ids.append(run_dir.name)

    raw_path = output_root / "matrix_results.csv"
    raw_fields = ["run_id"] + [key for key in rows[0] if key != "run_id"]
    with raw_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=raw_fields)
        writer.writeheader()
        writer.writerows(rows)

    summary_rows = summarize(rows)
    summary_path = output_root / "seed_summary.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)

    matrix_manifest = {
        "matrix_schema_version": "1",
        "run_count": len(run_dirs),
        "validated_raw_rows": len(rows),
        "candidates": sorted(CANDIDATES),
        "cache_states": sorted(CACHE_STATES),
        "operations": sorted(OPERATIONS),
        "workloads": [
            {"edges": edges, "distribution": distribution, "seed": seed}
            for edges, distribution, seed in sorted(workload_keys)
        ],
        "equivalence": "PASS",
        "seed_variability": (
            "Descriptive population standard deviation across available fixed seeds; "
            "not a confidence interval."
        ),
        "cache_limit": (
            "best-effort POSIX_FADV_DONTNEED only; no guaranteed cold-cache "
            "or cold-host measurements."
        ),
        "run_ids": run_ids,
        "outputs": {
            raw_path.name: raw_path.stat().st_size,
            summary_path.name: summary_path.stat().st_size,
        },
    }
    manifest_path = output_root / "matrix_manifest.json"
    manifest_path.write_text(
        json.dumps(matrix_manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(
        f"Validated {len(run_dirs)} runs / {len(rows)} rows; "
        f"wrote {len(summary_rows)} summary groups to {summary_path}."
    )


if __name__ == "__main__":
    main()
