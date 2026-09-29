#!/usr/bin/env python3
"""Compare interleaved corosio benchmark runs.

summarize: reduce raw per-iteration JSON (base/head × backend) to one
per-platform summary with flagging.
report: merge per-platform summaries into the PR comment markdown.

Input files: <side>-<backend>-<iter>.json as written by
corosio_bench --output. Never exits nonzero because of suite-shape
differences between base and head; only real I/O or usage errors fail.

Per benchmark, delta_pct is the median of the per-iteration paired
deltas (resists a single outlier iteration) and noise_pct is the
larger of the base and head sample CVs (a shift confined to one side
still sets a floor).
"""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

MIN_EFFECT_PCT = 2.0
NOISE_FACTOR = 3.0
HIGHER_BETTER = ("bytes_per_sec", "items_per_sec", "ops_per_sec")
FNAME = re.compile(r"^(base|head)-([A-Za-z0-9_]+)-(\d+)\.json$")


def primary_metric(category, metrics):
    """Pick the compared metric and its direction for one benchmark."""
    if "latency" in category and "latency_mean_ns" in metrics:
        return "latency_mean_ns", "lower"
    for m in HIGHER_BETTER:
        if m in metrics:
            return m, "higher"
    if "latency_mean_ns" in metrics:
        return "latency_mean_ns", "lower"
    return None, None


def human(value, metric):
    """Format a metric value with readable units."""
    if metric.endswith("_ns"):
        for factor, unit in ((1e9, "s"), (1e6, "ms"), (1e3, "µs")):
            if abs(value) >= factor:
                return f"{value / factor:.2f} {unit}"
        return f"{value:.0f} ns"
    # Bytes glue the prefix to the unit (KB/s, GB/s); count-style metrics
    # glue it to the number (1.82K ops/s) so sub-1000 values still carry a
    # unit instead of a bare "/s".
    if metric == "bytes_per_sec":
        for factor, prefix in ((1e9, "G"), (1e6, "M"), (1e3, "K")):
            if abs(value) >= factor:
                n = value / factor
                return (f"{n:.2f} {prefix}B/s" if n < 100
                        else f"{n:.1f} {prefix}B/s")
        return f"{value:.1f} B/s"
    unit = "items/s" if metric == "items_per_sec" else "ops/s"
    for factor, prefix in ((1e9, "G"), (1e6, "M"), (1e3, "K")):
        if abs(value) >= factor:
            n = value / factor
            return (f"{n:.2f}{prefix} {unit}" if n < 100
                    else f"{n:.1f}{prefix} {unit}")
    return f"{value:.1f} {unit}"


MARKER = "<!-- corosio-bench-report -->"
MAX_COMMENT_CHARS = 60000


FLAGGED_HEADER = "| Platform | Backend | Benchmark | Metric | Base | Head | Δ | Noise |"
FLAGGED_RULE = "|---|---|---|---|---|---|---|---|"
DETAIL_HEADER = "| Benchmark | Backend | Metric | Base | Head | Δ | Noise |"
DETAIL_RULE = "|---|---|---|---|---|---|---|"


def _delta_cells(r):
    # Deltas are sign-normalized (positive = improvement), so color tracks
    # verdict on every row, flagged or not. No green arrow exists in emoji,
    # so a colored dot carries the verdict and a text arrowhead the direction.
    # Color follows the two-decimal value actually shown, so a delta that
    # displays as 0.00% is neutral rather than a red "-0.00%".
    shown = round(r["delta_pct"], 2)
    if shown == 0:
        delta = "⚪ 0.00%"
    else:
        arrow = "🔴▼" if shown < 0 else "🟢▲"
        delta = f"{arrow} {shown:+.2f}%"
    noise = "—" if r["noise_pct"] is None else f"{r['noise_pct']:.2f}%"
    return (f"{human(r['base_mean'], r['metric'])} "
            f"| {human(r['head_mean'], r['metric'])} "
            f"| {delta} | {noise}")


def _flagged_line(platform, r):
    return (f"| {platform} | {r['backend']} | {r['category']}/{r['name']} "
            f"| {r['metric']} | {_delta_cells(r)} |")


def _detail_line(r):
    return f"| {r['name']} | {r['backend']} | {r['metric']} | {_delta_cells(r)} |"


def _platform_detail_lines(platform, s, condensed):
    """Full per-platform results table, or a one-line summary when condensed."""
    if condensed:
        counts = []
        if s["new"]:
            counts.append(f"{len(s['new'])} new")
        if s["removed"]:
            counts.append(f"{len(s['removed'])} removed")
        if s["unsupported"]:
            counts.append(f"{len(s['unsupported'])} unsupported")
        suffix = f" ({', '.join(counts)})" if counts else ""
        return [f"- **{platform}** — {len(s['rows'])} benchmarks, "
                f"{s['iterations']} iterations, {s['duration_s']}s each"
                f"{suffix}"]

    lines = [f"<details><summary>{platform} — full results "
             f"({len(s['rows'])} benchmarks, {s['iterations']} iterations, "
             f"{s['duration_s']}s each)</summary>", ""]
    # One table per category keeps rows narrow enough for GitHub's comment
    # width; same-name rows sort adjacently so backends compare at a glance.
    by_category = {}
    for r in s["rows"]:
        by_category.setdefault(r["category"], []).append(r)
    for category in sorted(by_category):
        lines += [f"**{category}**", "", DETAIL_HEADER, DETAIL_RULE]
        rows = sorted(by_category[category],
                      key=lambda r: (r["name"], r["backend"]))
        lines += [_detail_line(r) for r in rows]
        lines.append("")
    if s["new"]:
        lines += ["**New benchmarks (no baseline):**", ""]
        lines += [f"- `{n['category']}/{n['name']}` [{n['backend']}] "
                  f"{human(n['head_mean'], n['metric'])}" for n in s["new"]]
        lines.append("")
    if s["removed"]:
        lines += ["**Removed benchmarks:** " +
                  ", ".join(f"`{r['category']}/{r['name']}`"
                            for r in s["removed"]), ""]
    if s["unsupported"]:
        lines += ["**Unsupported (no recognized metric):** " +
                  ", ".join(f"`{u['category']}/{u['name']}`"
                            for u in s["unsupported"]), ""]
    lines += ["</details>", ""]
    return lines


def _build_report(summaries, base_sha, head_sha, run_url, condensed):
    lines = [MARKER, "## Benchmark report", ""]
    mode = next((s["mode"] for s in summaries.values() if s), "ab")
    if mode == "aa":
        lines += ["**A/A validation run** — base compared against itself; "
                  "every flag below is a false positive.", ""]
    lines += [f"`{base_sha[:12]}` (base) vs `{head_sha[:12]}` (head)", ""]

    for platform, s in summaries.items():
        if s is None:
            lines.append(f"- ❌ **{platform}** — no results "
                         "(job failed or runner offline)")
        elif s["flagged_count"]:
            lines.append(f"- ⚠️ **{platform}** — {s['flagged_count']} flagged "
                         f"({', '.join(s['backends'])})")
        else:
            lines.append(f"- ✅ **{platform}** — clean "
                         f"({', '.join(s['backends'])})")
    lines.append("")

    flagged = [(p, r) for p, s in summaries.items() if s
               for r in s["rows"] if r["flagged"]]
    if flagged:
        lines += ["### ⚠️ Flagged", "", FLAGGED_HEADER, FLAGGED_RULE]
        lines += [_flagged_line(p, r) for p, r in flagged]
        lines.append("")

    for platform, s in summaries.items():
        if s is None:
            continue
        lines += _platform_detail_lines(platform, s, condensed)

    if condensed:
        lines += ["_full tables omitted — comment size limit; "
                  "see the run artifacts_", ""]

    lines += [f"[Run & raw JSON artifacts]({run_url}) · "
              "flag rule: |median Δ| > max(2%, 3×CV of the noisier side) "
              "· advisory only"]
    return "\n".join(lines) + "\n"


def report(summaries, base_sha, head_sha, run_url):
    md = _build_report(summaries, base_sha, head_sha, run_url, condensed=False)
    # GitHub caps issue comments at 65536 chars; a run with many benchmarks
    # across three platforms can exceed that in full-details form.
    if len(md) > MAX_COMMENT_CHARS:
        md = _build_report(summaries, base_sha, head_sha, run_url, condensed=True)
    return md


def load_runs(input_dir):
    """Return {(side, backend, iter): {(category, name): {metric: value}}}."""
    runs = {}
    for p in sorted(Path(input_dir).iterdir()):
        m = FNAME.match(p.name)
        if not m:
            continue
        side, backend, it = m.group(1), m.group(2), int(m.group(3))
        try:
            payload = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"warning: skipping unreadable {p.name}: {e}", file=sys.stderr)
            continue
        if not isinstance(payload, dict):
            print(f"warning: skipping non-object JSON {p.name}", file=sys.stderr)
            continue
        benchmarks = payload.get("benchmarks", [])
        if not isinstance(benchmarks, list):
            print(f"warning: skipping {p.name}: benchmarks field is not a list", file=sys.stderr)
            continue
        table = {}
        for b in benchmarks:
            if not isinstance(b, dict):
                print(f"warning: skipping non-object benchmark entry in {p.name}", file=sys.stderr)
                continue
            key = (b.get("category", ""), b.get("name", ""))
            table[key] = {
                k: v for k, v in b.items()
                if isinstance(v, (int, float)) and not isinstance(v, bool)
            }
        runs[(side, backend, it)] = table
    return runs


def _values(runs, side, backend, key, metric):
    """Metric samples for one benchmark on one side, ordered by iteration."""
    out = []
    for (s, b, it), table in sorted(runs.items(), key=lambda kv: kv[0][2]):
        if s == side and b == backend and key in table and metric in table[key]:
            out.append((it, table[key][metric]))
    return out


def summarize(input_dir, platform, mode="ab"):
    runs = load_runs(input_dir)
    backends = sorted({b for (_, b, _) in runs})
    iterations = max((it for (_, _, it) in runs), default=0)
    duration = 0.0
    rows, new, removed, unsupported = [], [], [], []

    for backend in backends:
        base_keys, head_keys = set(), set()
        sample = {}
        for (s, b, it), table in runs.items():
            if b != backend:
                continue
            (base_keys if s == "base" else head_keys).update(table)
            for key, metrics in table.items():
                sample.setdefault(key, metrics)

        for key in sorted(base_keys | head_keys):
            category, name = key
            metric, direction = primary_metric(category, sample.get(key, {}))
            if metric is None:
                unsupported.append(
                    {"backend": backend, "category": category, "name": name})
                continue
            if key not in base_keys:
                head = _values(runs, "head", backend, key, metric)
                mean = statistics.fmean(v for _, v in head) if head else 0.0
                new.append({"backend": backend, "category": category,
                            "name": name, "metric": metric, "head_mean": mean})
                continue
            if key not in head_keys:
                removed.append(
                    {"backend": backend, "category": category, "name": name})
                continue

            base = dict(_values(runs, "base", backend, key, metric))
            head = dict(_values(runs, "head", backend, key, metric))
            common = sorted(set(base) & set(head))
            deltas = []
            for it in common:
                b_v, h_v = base[it], head[it]
                if b_v == 0:
                    continue
                d = (h_v - b_v) / b_v * 100.0
                if direction == "lower":
                    d = -d
                deltas.append(d)
            if not deltas:
                unsupported.append(
                    {"backend": backend, "category": category, "name": name})
                continue

            base_vals = [base[it] for it in common]
            head_vals = [head[it] for it in common]
            base_mean = statistics.fmean(base_vals)
            head_mean = statistics.fmean(head_vals)

            def cv(vals, mean):
                if len(vals) >= 2 and mean != 0:
                    return statistics.stdev(vals) / abs(mean) * 100.0
                return None

            base_cv = cv(base_vals, base_mean)
            head_cv = cv(head_vals, head_mean)
            # a single-side outlier or a side-level shift can leave one
            # side's spread tight while the other carries the noise
            noise_candidates = [c for c in (base_cv, head_cv) if c is not None]
            noise_pct = max(noise_candidates) if noise_candidates else None
            # median resists a single blown-up iteration that a mean would not
            delta_pct = statistics.median(deltas)
            flagged = (
                noise_pct is not None
                and abs(delta_pct) > max(MIN_EFFECT_PCT, NOISE_FACTOR * noise_pct)
            )
            rows.append({
                "backend": backend, "category": category, "name": name,
                "metric": metric, "direction": direction,
                "base_mean": base_mean, "head_mean": head_mean,
                "delta_pct": delta_pct, "noise_pct": noise_pct,
                "flagged": flagged,
            })

    for p in Path(input_dir).iterdir():
        if FNAME.match(p.name):
            try:
                duration = json.loads(p.read_text())["metadata"]["duration_s"]
                break
            except Exception:
                pass

    return {
        "platform": platform, "backends": backends,
        "iterations": iterations, "duration_s": duration, "mode": mode,
        "rows": rows, "new": new, "removed": removed,
        "unsupported": unsupported,
        "flagged_count": sum(1 for r in rows if r["flagged"]),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(prog="compare.py")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summarize")
    s.add_argument("--platform", required=True)
    s.add_argument("--input-dir", required=True)
    s.add_argument("--output", required=True)
    s.add_argument("--mode", default="ab", choices=("ab", "aa"))
    r = sub.add_parser("report")
    r.add_argument("--summaries", required=True,
                   help="dir containing bench-<platform>/summary.json")
    r.add_argument("--expect", required=True,
                   help="comma-separated platform list")
    r.add_argument("--base-sha", required=True)
    r.add_argument("--head-sha", required=True)
    r.add_argument("--run-url", required=True)
    r.add_argument("--output", required=True)
    args = ap.parse_args(argv)

    if args.cmd == "summarize":
        summary = summarize(args.input_dir, args.platform, args.mode)
        Path(args.output).write_text(json.dumps(summary, indent=2))
        print(f"{args.platform}: {len(summary['rows'])} rows, "
              f"{summary['flagged_count']} flagged")
    elif args.cmd == "report":
        summaries = {}
        for platform in args.expect.split(","):
            p = Path(args.summaries) / f"bench-{platform}" / "summary.json"
            try:
                summaries[platform] = json.loads(p.read_text())
            except (OSError, json.JSONDecodeError):
                summaries[platform] = None
        md = report(summaries, args.base_sha, args.head_sha, args.run_url)
        Path(args.output).write_text(md)
        print(f"report written: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
