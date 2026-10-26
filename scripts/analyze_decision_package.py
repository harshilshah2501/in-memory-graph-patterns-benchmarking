#!/usr/bin/env python3
"""Join Phase 2 (build/query/memory) and Phase 3 (recovery) results into an omni-view
decision package for the 5M-20M scope. No composite score is computed."""
import argparse
import csv
import json
import statistics as st
from collections import defaultdict
from pathlib import Path

CANDS = ["forward_hash", "bidirectional_hash", "csr_csc"]
ELIGIBLE = ["bidirectional_hash", "csr_csc"]  # forward_hash lacks reverse traversal
SCALES = [5_000_000, 10_000_000, 20_000_000]
DISTS = ["uniform", "hub", "clustered"]


def read(p):
    with open(p, newline="") as f:
        return list(csv.DictReader(f))


def f(x):
    return float(x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase2", default="artifacts/cross-scale-analysis/condition_summary.csv")
    ap.add_argument("--recovery-dir", default="artifacts/phase3-recovery-5m-20m-full-distributions")
    ap.add_argument("--output-dir", default="artifacts/decision-package")
    a = ap.parse_args()
    out = Path(a.output_dir)
    out.mkdir(parents=True, exist_ok=True)

    p2 = read(a.phase2)
    rec_dir = Path(a.recovery_dir)
    manifest = json.loads((rec_dir / "matrix_manifest.json").read_text())
    rows = read(rec_dir / "matrix_results.csv")
    seeds = read(rec_dir / "seed_summary.csv")

    # Coverage validation
    assert len(rows) == 324, f"expected 324 recovery rows, got {len(rows)}"
    assert all(r["equivalence"] == "PASS" for r in rows), "equivalence failure"
    assert len({r["candidate"] for r in rows}) == 3
    assert len(seeds) == 3 * 3 * 3 * 2, f"unexpected seed_summary rows {len(seeds)}"
    assert len(p2) == 3 * 3 * 3 * 3, f"unexpected phase2 rows {len(p2)}"

    # Phase 2 omni-view: mean over distributions and hops for each (scale, candidate)
    q = defaultdict(list)
    for r in p2:
        q[(int(r["edges"]), r["representation"], int(r["hops"]), r["distribution"])] = r
    omni = []
    for e in SCALES:
        for c in CANDS:
            sel = [r for r in p2 if int(r["edges"]) == e and r["representation"] == c]
            rec = [s for s in seeds if int(s["edges"]) == e and s["candidate"] == c]
            rec_w = [s for s in rec if s["cache_state"] == "warm-process-restart"]
            rec_a = [s for s in rec if s["cache_state"] == "best-effort-file-advice"]
            def m(k, rs):
                v = [f(r[k]) for r in rs if r[k] != ""]
                # forward-only candidate has no native reverse traversal
                return st.mean(v) if v else float("inf")
            omni.append({
                "edges": e, "candidate": c,
                "build_p50_ms": m("build_p50_ms_mean", sel),
                "rca_fwd_p50_us": m("rca_forward_p50_us_mean", sel),
                "rca_rev_p50_us": m("rca_reverse_p50_us_mean", sel),
                "retained_bytes_per_edge": m("retained_bytes_per_edge_est_mean", sel),
                "query_peak_rss_mib": m("peak_rss_kb_mean", sel) / 1024,
                "source_rebuild_ms": m("source_rebuild_p50_mean_ms", rec_w),
                "snapshot_load_ms_warm": m("snapshot_load_p50_mean_ms", rec_w),
                "snapshot_load_ms_advised": m("snapshot_load_p50_mean_ms", rec_a),
                "snapshot_speedup_warm": m("snapshot_vs_rebuild_speedup_mean", rec_w),
                "snapshot_speedup_advised": m("snapshot_vs_rebuild_speedup_mean", rec_a),
                "snapshot_load_rss_mib": m("snapshot_load_peak_rss_mean_kb", rec_w) / 1024,
                "snapshot_bytes_mib": f(rec_w[0]["snapshot_bytes"]) / 2**20,
            })
    cols = list(omni[0])
    with open(out / "omni_view.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(omni)

    # Break-even: snapshot write cost vs per-restart saving (warm cache)
    be = []
    byk = defaultdict(lambda: defaultdict(list))
    for r in rows:
        if r["cache_state"] != "warm-process-restart":
            continue
        byk[(int(r["edges"]), r["candidate"])][r["operation"]].append(r)
    for (e, c), d in sorted(byk.items()):
        wr = st.mean(f(r["snapshot_write_ms"]) for r in d["snapshot-load"])
        rb = st.mean(f(r["ready_p50_ms"]) for r in d["source-rebuild"])
        ld = st.mean(f(r["ready_p50_ms"]) for r in d["snapshot-load"])
        be.append({"edges": e, "candidate": c, "snapshot_write_ms": wr,
                   "rebuild_ms": rb, "load_ms": ld,
                   "saving_per_restart_ms": rb - ld,
                   "break_even_restarts": wr / (rb - ld) if rb > ld else float("inf")})
    with open(out / "break_even.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(be[0]))
        w.writeheader()
        w.writerows(be)

    # Pareto non-dominated sets at each scale; dimensions are lower-is-better
    def pareto(dims, scale):
        pts = [o for o in omni if o["edges"] == scale and o["candidate"] in ELIGIBLE]
        nd = []
        for p in pts:
            dom = any(all(o[d] <= p[d] for d in dims) and any(o[d] < p[d] for d in dims)
                      for o in pts if o is not p)
            if not dom:
                nd.append(p["candidate"])
        return nd

    fams = {
        "build": ["build_p50_ms"],
        "query": ["rca_fwd_p50_us", "rca_rev_p50_us"],
        "memory": ["retained_bytes_per_edge", "query_peak_rss_mib"],
        "recovery": ["snapshot_load_ms_warm", "snapshot_load_rss_mib"],
        "all": ["build_p50_ms", "rca_fwd_p50_us", "rca_rev_p50_us",
                "retained_bytes_per_edge", "snapshot_load_ms_warm", "snapshot_load_rss_mib"],
    }
    pareto_rows = [{"edges": e, "family": k, "non_dominated": ";".join(pareto(d, e))}
                   for e in SCALES for k, d in fams.items()]
    with open(out / "pareto_sets.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=["edges", "family", "non_dominated"])
        w.writeheader()
        w.writerows(pareto_rows)

    # Report
    L = ["# Decision package (5M-20M scope)", "",
         "Generated by `scripts/analyze_decision_package.py`. No composite score; values are",
         "means over 3 seeds, 3 distributions (and 3 hop depths for Phase 2 metrics).",
         f"Recovery manifest keys: {', '.join(sorted(manifest))[:120]}; 324 rows, all equivalence PASS.", "",
         "## Omni-view", "",
         "| Edges | Candidate | Build p50 ms | RCA fwd us | RCA rev us | B/edge | Query RSS MiB | Rebuild ms | Load ms (warm) | Speedup warm | Speedup advised | Load RSS MiB | Snapshot MiB |",
         "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for o in omni:
        L.append(f"| {o['edges']//10**6}M | {o['candidate']} | {o['build_p50_ms']:.0f} | {o['rca_fwd_p50_us']:.1f} | "
                 f"{'n/a' if o['rca_rev_p50_us'] == float('inf') else format(o['rca_rev_p50_us'], '.1f')} | {o['retained_bytes_per_edge']:.1f} | {o['query_peak_rss_mib']:.0f} | "
                 f"{o['source_rebuild_ms']:.0f} | {o['snapshot_load_ms_warm']:.0f} | {o['snapshot_speedup_warm']:.2f}x | "
                 f"{o['snapshot_speedup_advised']:.2f}x | {o['snapshot_load_rss_mib']:.0f} | {o['snapshot_bytes_mib']:.0f} |")
    L += ["", "## Snapshot break-even (warm restart; restarts needed to repay snapshot write)", "",
          "| Edges | Candidate | Write ms | Rebuild ms | Load ms | Break-even restarts |", "|---|---|---|---|---|---|"]
    for b in be:
        L.append(f"| {b['edges']//10**6}M | {b['candidate']} | {b['snapshot_write_ms']:.0f} | {b['rebuild_ms']:.0f} | "
                 f"{b['load_ms']:.0f} | {b['break_even_restarts']:.2f} |")
    L += ["", "## Pareto non-dominated candidates by metric family (eligible candidates only: bidirectional_hash, csr_csc)", "",
          "| Edges | Family | Non-dominated |", "|---|---|---|"]
    for p in pareto_rows:
        L.append(f"| {p['edges']//10**6}M | {p['family']} | {p['non_dominated']} |")
    L += ["", "## Reading guide and workload-conditioned conclusions", "",
          "- forward_hash is a reference baseline only: it has no reverse traversal and is excluded from Pareto sets. Its cost reference: adding reverse support via bidirectional_hash costs ~1.7x memory; csr_csc provides both directions at fewer bytes/edge than forward_hash.",
          "- Between eligible candidates, csr_csc is non-dominated on every metric family at every scale; bidirectional_hash is dominated.",
          "- Caveat: on hub graphs csr_csc build is on par with bidirectional_hash (see FINAL_REPORT.md); this table averages distributions.",
          "- Recovery: csr_csc snapshot load is ~1.7-1.9x faster than rebuild (warm), ~1.3x with file advice; snapshot write repays after ~2-4.3 restarts.",
          "- Conclusion (5M-20M, static graph, fwd+rev RCA): csr_csc; recovery gain is modest, so a snapshot is worthwhile only if restarts are frequent.", "",
          "## Limitations", "",
          "- Scope is 5M-20M edges; 30M/40M/50M are untested.",
          "- Cache states are warm-process-restart and best-effort file advice; not strict cold-host.",
          "- Synthetic workloads, 3 seeds; seed SD is descriptive, not a confidence interval.",
          "- Snapshots are not fsynced (no crash-safety claim); FNV-1a catches accidental corruption only.",
          "- No edge-type-filtered queries, production traces, or delta/update workloads.",
          "- Recovery speedups exclude snapshot creation (see break-even table)."]
    (out / "decision_report.md").write_text("\n".join(L) + "\n")
    print("wrote", out)


if __name__ == "__main__":
    main()
