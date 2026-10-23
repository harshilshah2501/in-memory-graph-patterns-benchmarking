#!/usr/bin/env python3
"""Summarize numeric benchmark metrics across dataset seeds."""

import argparse
import csv
import math
from pathlib import Path


GROUP_COLUMNS = (
    "representation",
    "edges",
    "vertices",
    "distribution",
    "queries",
    "hops",
    "trials",
    "warmups",
)
EXCLUDED_NUMERIC_COLUMNS = {
    "seed",
    "one_hop_forward_checksum",
    "one_hop_reverse_checksum",
    "rca_forward_checksum",
    "rca_reverse_checksum",
    "rss_exit_code",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    with args.input.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or not set(GROUP_COLUMNS).issubset(reader.fieldnames):
            raise ValueError("matrix CSV is missing required grouping columns")
        rows = list(reader)

    if not rows:
        raise ValueError("matrix CSV contains no data rows")
    if any(row.get("schema_version") != "v2" for row in rows):
        raise ValueError("matrix CSV contains an unsupported schema version")

    metric_columns = []
    for column in rows[0]:
        if column in GROUP_COLUMNS or column in EXCLUDED_NUMERIC_COLUMNS:
            continue
        values = [row[column] for row in rows if row.get(column, "") != ""]
        if values:
            try:
                for value in values:
                    float(value)
            except ValueError:
                continue
            metric_columns.append(column)

    groups = {}
    for row in rows:
        key = tuple(row[column] for column in GROUP_COLUMNS)
        groups.setdefault(key, []).append(row)

    output_columns = list(GROUP_COLUMNS) + ["seed_count", "seeds"]
    for column in metric_columns:
        output_columns.extend(
            (f"{column}_mean_across_seeds", f"{column}_stddev_across_seeds")
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as destination:
        writer = csv.DictWriter(destination, fieldnames=output_columns)
        writer.writeheader()
        for key, group in sorted(groups.items()):
            output = dict(zip(GROUP_COLUMNS, key))
            seeds = sorted({int(row["seed"]) for row in group})
            output["seed_count"] = len(seeds)
            output["seeds"] = " ".join(str(seed) for seed in seeds)
            for column in metric_columns:
                values = [float(row[column]) for row in group if row[column] != ""]
                if values:
                    mean = sum(values) / len(values)
                    variance = sum((value - mean) ** 2 for value in values) / len(values)
                    output[f"{column}_mean_across_seeds"] = mean
                    output[f"{column}_stddev_across_seeds"] = math.sqrt(variance)
                else:
                    output[f"{column}_mean_across_seeds"] = ""
                    output[f"{column}_stddev_across_seeds"] = ""
            writer.writerow(output)

    print(f"Seed summary written: {args.output}")


if __name__ == "__main__":
    main()
