#!/usr/bin/env python3
"""Generate the published benchmark report pages and their charts.

Consumes the artifacts of the benchmark-report workflow (one directory per
platform holding <config>-<iter>.json raw runs plus environment.json) and
emits a landing page plus one page per platform, and one diverging-bar SVG
per category per platform. The pages are fully generated: nothing on them
is hand-maintained.

Metric selection and direction rules mirror .github/bench/compare.py; the
duplication is deliberate so each script stays single-file.
"""
import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path

BASELINE = "asio_callback"
HIGHER_BETTER = ("bytes_per_sec", "items_per_sec", "ops_per_sec")
FNAME = re.compile(r"^(?P<config>[a-z_]+(?:-[a-z]+)?)-(?P<iter>\d+)\.json$")

# Series order and display names are fixed per platform; colors are fixed per
# entity and never cycled. Palette validated with the dataviz skill's
# validate_palette.js (light surface, 2026-09-30: all checks pass; the aqua
# contrast WARN is relieved by direct labels and the exact-value tables).
#
# On Linux, asio is built twice (epoll and io_uring reactors) so every
# comparison can pair implementations on the SAME reactor. A multi-backend
# platform's Results section charts each corosio backend on its own
# (see write_outputs/build_platform_page): one chart per category per
# backend, its series just that backend's corosio config plus its
# matched asio flavors, so a reader comparing epoll never has
# io_uring bars (or the other reactor's callback flavor) sharing the
# plot. PLATFORM_SERIES itself stays the flat per-platform list: it still
# drives the Detailed results table's row order and, on a single-backend
# platform, is used as-is for that platform's one chart per category.
PLATFORM_SERIES = {
    "linux": ["corosio-uring", "corosio-epoll", "asio-uring"],
    "windows": ["corosio-iocp", "asio"],
    "macos": ["corosio-kqueue", "asio"],
}
SERIES_LABEL = {
    "corosio-epoll": "corosio (epoll)", "corosio-uring": "corosio (io_uring)",
    "corosio-iocp": "corosio (IOCP)", "corosio-kqueue": "corosio (kqueue)",
    "asio_callback": "asio (callbacks)", "asio": "asio (coroutines)",
    "asio_callback-epoll": "asio (callbacks, epoll)",
    "asio_callback-uring": "asio (callbacks, io_uring)",
    "asio-epoll": "asio (coroutines, epoll)",
    "asio-uring": "asio (coroutines, io_uring)",
}
PLATFORM_DISPLAY = {"linux": "Linux", "windows": "Windows", "macos": "macOS"}
BACKEND_DISPLAY = {"epoll": "epoll", "uring": "io_uring", "iocp": "IOCP",
                   "kqueue": "kqueue"}


def platform_display(platform):
    return PLATFORM_DISPLAY.get(platform, platform.capitalize())


def backend_display(config):
    """Return the human-readable backend name for a `corosio-<backend>`
    config (e.g. 'io_uring' for 'corosio-uring', 'IOCP' for
    'corosio-iocp')."""
    suffix = config[len("corosio-"):] if config.startswith("corosio-") else config
    return BACKEND_DISPLAY.get(suffix, suffix)


def series_color(config):
    if config == "corosio-uring":
        return "#eb6834"
    if config.startswith("corosio"):
        return "#2a78d6"
    return "#1baf7a"


def series_class(config):
    if config == "corosio-uring":
        return "bch-s-uring"
    if config.startswith("corosio"):
        return "bch-s-native"
    return "bch-s-cb"


# SVG chart rendering constants
CHART_W = 720
LABEL_W = 230
LABEL_ROOM = 48    # px reserved each side of the plot for value-label text
BAR_H, BAR_GAP, GROUP_GAP = 12, 2, 10
MARGIN_T, MARGIN_B, LEGEND_H = 36, 34, 22
INK, INK2, GRID = "#1f1f1e", "#5f5e58", "#e3e2dc"
SURFACE = "#fcfcfb"
FONT = "font-family='-apple-system,Segoe UI,Helvetica,Arial,sans-serif'"

# Dark counterparts of the light colors above, validated with the dataviz
# skill's validate_palette.js (dark surface, 2026-09-30: all checks pass;
# series hues carried over from light, only lightened to stay legible on
# a dark surface).
DARK_SURFACE = "#1a1a19"
DARK_INK, DARK_INK2, DARK_GRID = "#ffffff", "#c3c2b7", "#3a3a38"

# Diverging poles for the per-platform summary chart (faster/slower), plus
# a deliberately gray, unvalidated neutral for "within noise". Poles
# validated with the dataviz skill's validate_palette.js as a 2-slot
# categorical pair: light "#2a78d6,#e34948" (surface #fcfcfb) and dark
# "#3987e5,#e66767" (surface #0d0e0f, the site's actual dark background,
# since the chart surface is transparent there) — both ALL PASS,
# 2026-09-30.
DIV_POS, DIV_POS_DARK = "#2a78d6", "#3987e5"
DIV_NEG, DIV_NEG_DARK = "#e34948", "#e66767"
DIV_MID, DIV_MID_DARK = "#f0efec", "#383835"

# (class, property, light value, dark value); "fill" classes set fill, the
# two line classes set stroke instead.
_THEME_RULES = [
    ("bch-surface", "fill", SURFACE, DARK_SURFACE),
    ("bch-ink", "fill", INK, DARK_INK),
    ("bch-ink2", "fill", INK2, DARK_INK2),
    ("bch-grid", "stroke", GRID, DARK_GRID),
    ("bch-zero", "stroke", INK2, DARK_INK2),
    ("bch-s-native", "fill", "#2a78d6", "#3987e5"),
    ("bch-s-uring", "fill", "#eb6834", "#d95926"),
    ("bch-s-cb", "fill", "#1baf7a", "#199e70"),
    ("bch-div-pos", "fill", DIV_POS, DIV_POS_DARK),
    ("bch-div-neg", "fill", DIV_NEG, DIV_NEG_DARK),
    ("bch-div-mid", "fill", DIV_MID, DIV_MID_DARK),
]


def _rules(selector_fmt, value_index):
    return "".join(f"{selector_fmt.format(cls)}{{{prop}:{vals[value_index]}}}"
                    for cls, prop, *vals in _THEME_RULES)


def _site_rules(selector_fmt, value_index):
    """Like `_rules`, but bch-surface goes transparent.

    Once inlined, the chart sits directly on the page's own background,
    which this SVG has no way to know the exact value of — going
    transparent makes it match exactly instead of overlaying a baked-in
    guess that can drift from the site's actual token. Not used for the
    `prefers-color-scheme` block: a standalone-opened SVG has no page
    background to match, so it keeps its own solid surface color there.
    """
    parts = []
    for cls, prop, *vals in _THEME_RULES:
        v = "transparent" if cls == "bch-surface" else vals[value_index]
        parts.append(f"{selector_fmt.format(cls)}{{{prop}:{v}}}")
    return "".join(parts)


# The docs site toggles dark mode with an `html.dark` class rather than
# `prefers-color-scheme`, so an <img>-loaded SVG (which never sees the host
# page's class list) can't follow it — the chart must be inlined and carry
# its own theme-aware CSS. Cascade order matters: the media query covers a
# standalone SVG opened outside any page (no `html` ancestor to match
# against); `html:not(.dark)` then re-asserts light so a site explicitly
# set to light beats an OS set to dark; `html.dark` comes last so the
# site's own dark toggle has final say over that re-assertion. CSS always
# beats the presentation attributes on the elements themselves, so none of
# this needs `!important`, and the attributes stand as the light-mode
# fallback for renderers that ignore <style> entirely.
#
# Ink, grid, and series colors are unaffected by the surface going
# transparent in site context: they were validated against the #1a1a19
# reference dark surface, and the site's actual dark background
# (neutral-950) is darker still, so lightness separation only increases.
CHART_STYLE = (
    "<style>"
    # Fluid sizing: the width/height attributes on <svg> are the no-CSS
    # fallback; where CSS applies, this overrides them so the chart scales
    # with its container while viewBox keeps the aspect ratio fixed.
    # overflow:visible is the safety net, not the mechanism: label
    # positions are clamped in Python so nothing SHOULD reach the edge,
    # but this overrides the SVG UA default of clipping at the viewBox
    # so any position that still pokes past it renders as a harmless
    # overhang instead of silently disappearing.
    ".bch-chart{width:100%;height:auto;overflow:visible}"
    # The docs theme centers an imageblock with a flex column that
    # shrink-wraps its .content div to the image's fallback width, so our
    # 100% above was resolving against the SVG's own 720px attribute
    # instead of the real column. Stretching only our role-tagged blocks
    # lets the percentage resolve against the actual content width.
    ".imageblock.bch-block{align-items:stretch}"
    ".imageblock.bch-block .content{width:100%}"
    # In-segment count labels on the summary chart's pole (faster/slower)
    # segments: plain white, bold for legibility, no light/dark split of
    # its own. The palette validation above checked the pole fills
    # against the page surface, not white text against the fills
    # directly — untested here, but each count is also redundantly shown
    # in the stat line above and the exact-value tables in Results.
    ".bch-seg-label{fill:#ffffff;font-weight:700}"
    # Antora's collapsible block (the Detailed results table) emits a
    # bare <details><summary class="title">, and that summary label falls
    # outside the theme's own dark-mode rules, leaving black-on-dark
    # text. Only the summary needs this patch — the table body inside is
    # plain markup that already picks up the theme's dark-mode colors on
    # its own, verified fine without a td/th patch.
    "html.dark .bch-exact>summary.title,"
    "html.dark .bch-exact summary.title"
    "{color:var(--text-main-text-primary,#ffffff)}"
    # Each chart and its Detailed results collapsible share one bordered
    # card, so ownership is structural (common region) rather than
    # whitespace guesswork. The border token follows the site theme.
    # (.boostlook .doc prefixes outgun the theme's own three-class
    # margin:revert selector; weaker versions lose the cascade.)
    # Vertical rhythm hangs on the card itself (top and bottom), so a
    # backend label, category description, or sibling card above it gets
    # the same clearance without per-neighbor rules.
    ".boostlook .doc .openblock.bch-card"
    "{border:1px solid var(--border-border-secondary,#e3e2dc);"
    "border-radius:10px;padding:14px 14px 4px;"
    "margin-top:1.4rem;margin-bottom:1.4rem}"
    ".boostlook .doc .bch-card .imageblock.bch-block"
    "{margin-top:0;margin-bottom:0.25rem}"
    # The theme draws the summary's disclosure marker 1rem LEFT of the
    # details box (position:absolute;left:-1rem) while details itself
    # only gets margin-left:1rem — flush against the card border. The
    # extra margin re-insets marker and label comfortably inside it.
    # Inset only the summary line (its disclosure marker hangs 1rem to
    # the left); the content below stays at card padding so a stretched
    # table fits without horizontal overflow.
    ".boostlook .doc .bch-card details.bch-exact"
    "{margin-top:0;margin-bottom:0.5rem;margin-left:0}"
    ".boostlook .doc .bch-card details.bch-exact>summary"
    "{margin-left:2rem}"
    # An autowidth table cannot shrink below its content's natural
    # width; compact type and padding keep that minimum inside the card
    # so the wrapper never grows a horizontal scrollbar.
    # The theme hands tables a 2rem left margin through an
    # #content-scoped rule; matching the id in the selector outranks it
    # so the stretched table truly spans the card instead of scrolling.
    ".boostlook .doc .bch-card table.stretch,"
    ".boostlook #content .bch-card table.stretch"
    "{width:100%;margin-left:0;font-size:0.88em}"
    ".boostlook .doc .bch-card table.stretch td,"
    ".boostlook .doc .bch-card table.stretch th"
    "{padding:0.3rem 0.55rem}"

    "@media (prefers-color-scheme: dark) {" + _rules(".{}", 1) + "}"
    + _site_rules("html:not(.dark) .{}", 0)
    + _site_rules("html.dark .{}", 1)
    + "</style>"
)


def _esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def _axis_bound(values):
    m = max((abs(v) for v in values), default=5.0)
    for b in (5, 10, 15, 20, 30, 40, 50):
        if m <= b:
            return float(b)
    return 50.0


def _format_pct(v):
    """Format percentage, handling -0% → 0%."""
    s = f"{v:+.0f}%"
    return "0%" if s == "-0%" else s


def _format_pct1(v):
    """Format percentage to one decimal, handling -0.0% → 0.0%."""
    s = f"{v:+.1f}%"
    return "0.0%" if s == "-0.0%" else s


# Conservative px/char at font-size 10-11 (real glyph widths vary and can
# run wider than a naive average estimate) — clamping against this is a
# guarantee, not an estimate: the label may end up overlapping its own
# bar or another label when clamped, but a legible, slightly-overlapping
# label beats one silently clipped off the canvas.
LABEL_CHAR_W = 8


def _clamp_start(tx, text):
    """Clamp an anchor='start' label so it can't run off the right edge."""
    return min(tx, CHART_W - LABEL_CHAR_W * len(text) - 2)


def _clamp_end(tx, text):
    """Clamp an anchor='end' label so it can't run off the left edge."""
    return max(tx, LABEL_CHAR_W * len(text) + 2)


def _clamp_middle(cx, text):
    """Clamp an anchor='middle' label so it can't run off either edge."""
    half = LABEL_CHAR_W * len(text) / 2.0
    return min(max(cx, half + 2), CHART_W - half - 2)


def base_family(name):
    """Related-type key: the family prefix before any '/N' parameter,
    with a `_lockless` suffix folded into its base so a family and its
    lockless twin always land on the same chart."""
    fam = name.split("/", 1)[0]
    if fam.endswith("_lockless"):
        fam = fam[: -len("_lockless")]
    return fam


MAX_CHART_GROUPS = 12   # benchmarks per chart before a category splits


def split_benchmarks(names):
    """Partition sorted benchmark names into chart-sized runs.

    Families (see `base_family`) are never split across charts; runs
    pack greedily in name order until the next family would overflow
    MAX_CHART_GROUPS. A single family larger than the cap gets its own
    oversized chart rather than being broken up.
    """
    fams = []
    for n in names:
        key = base_family(n)
        if fams and fams[-1][0] == key:
            fams[-1][1].append(n)
        else:
            fams.append((key, [n]))
    parts, cur = [], []
    for _, ns in fams:
        if cur and len(cur) + len(ns) > MAX_CHART_GROUPS:
            parts.append(cur)
            cur = []
        cur.extend(ns)
    if cur:
        parts.append(cur)
    return parts


def chart_file_names(base, names):
    """The SVG file name(s) `render_chart` writes for this benchmark
    set: the plain `<base>.svg` when one chart suffices, otherwise
    `<base>-g1.svg` ... in `split_benchmarks` order. Shared with the
    page builder so emission never guesses at file names."""
    parts = split_benchmarks(sorted(names))
    if len(parts) <= 1:
        return [f"{base}.svg"]
    return [f"{base}-g{i}.svg" for i in range(1, len(parts) + 1)]


# Vertical-column chart geometry
VMARGIN_L = 52          # px for the y-axis percent labels
VMARGIN_R = 16
VPLOT_H = 220           # fixed plot height; width stays CHART_W
COL_GAP = 2             # px between a group's two columns
VGROUP_GAP = 12         # px between benchmark groups
X_TICK_CHAR_W = 5.4     # ~10px-font char width, for rotated tick room


def render_chart(platform, category, agg_subset, series, out_base,
                 descs=None, backend=None):
    """Write this category's vertical-column SVG chart(s).

    Columns rise (faster) or fall (slower) from a horizontal baseline
    representing Boost.Asio (callbacks) on the same reactor. Categories
    holding more than MAX_CHART_GROUPS benchmarks split into several
    charts along related-family lines (see `split_benchmarks`), named
    `<out_base>-g1.svg` and so on; smaller categories keep the single
    `<out_base>.svg`. Returns the list of paths written.
    """
    names = sorted(agg_subset)
    if not names:
        return []
    parts = split_benchmarks(names)
    fnames = chart_file_names(out_base.name, names)
    written = []
    for i, (part, fname) in enumerate(zip(parts, fnames), 1):
        out = out_base.parent / fname
        out.unlink(missing_ok=True)
        _render_chart_part(platform, category, agg_subset, part, series,
                           out, descs or {}, backend,
                           part_no=i if len(parts) > 1 else None)
        if out.exists():
            written.append(out)
    return written


def _nice_step(span):
    """A tick step from the 1/2/2.5/5 family giving ~4 intervals."""
    raw = span / 4.0
    mag = 10.0 ** math.floor(math.log10(raw)) if raw > 0 else 1.0
    for m in (1, 2, 2.5, 5, 10):
        if raw <= m * mag:
            return m * mag
    return 10 * mag


def _render_chart_part(platform, category, agg_subset, names, series,
                       out_path, descs, backend, part_no=None):
    rows = [(n, [(c, agg_subset[n]["rel"].get(c)) for c in series
                 if c in agg_subset[n]["rel"]]) for n in names]
    rows = [(n, bars) for n, bars in rows if bars]
    if not rows:
        return
    all_vals = [v for _, bars in rows for _, v in bars]
    # Axis fitted to the data so the bars fill the plot: pad past the
    # extremes, snap to a tick grid, and always include the baseline.
    vmax = max(max(all_vals), 0.0)
    vmin = min(min(all_vals), 0.0)
    pad = max((vmax - vmin) * 0.15, 1.0)
    step = _nice_step((vmax - vmin) + 2 * pad)
    hi = math.ceil((vmax + pad) / step) * step
    lo = math.floor((vmin - pad) / step) * step
    lower_better = any(agg_subset[n]["direction"] == "lower"
                       for n, _ in rows)

    plot_left = VMARGIN_L
    plot_right = CHART_W - VMARGIN_R
    plot_w = plot_right - plot_left
    top = MARGIN_T + LEGEND_H
    def y(v):
        return top + VPLOT_H - (min(max(v, lo), hi) - lo) \
            / (hi - lo) * VPLOT_H
    y0 = y(0.0)                          # the baseline

    n = len(rows)
    gw = (plot_w - VGROUP_GAP * (n - 1)) / n
    k = max(len(bars) for _, bars in rows)
    col_w = min(26.0, (gw - COL_GAP * (k - 1)) / k)
    group_pad = (gw - (col_w * k + COL_GAP * (k - 1))) / 2.0

    # Rotated tick labels need vertical room proportional to the longest
    # benchmark name; sin(38 deg) ~ 0.62.
    max_chars = max(len(nm) for nm, _ in rows)
    tick_h = min(130, max(36, int(0.62 * max_chars * X_TICK_CHAR_W) + 20))
    height = top + VPLOT_H + tick_h + 10
    label_all = n <= 8
    ext = {}
    for _, bars in rows:
        for c, v in bars:
            elo, ehi = ext.get(c, (v, v))
            ext[c] = (min(elo, v), max(ehi, v))

    # The visible title lives in the page heading above the chart; this
    # <title> is the SVG's accessible name and must be the first child.
    # The backend is named once in the hint line; series and baseline
    # labels stay backend-free so the legend doesn't repeat it.
    bdisp = backend or next(
        (backend_display(c) for c in series if c.startswith("corosio")), None)
    plat = _esc(platform_display(platform))
    title = (f"{_esc(category)} ({_esc(backend)}) — {plat}" if backend
             else f"{_esc(category)} — {plat}")
    if part_no:
        title += f", part {part_no}"
    s = [f"<svg xmlns='http://www.w3.org/2000/svg' class='bch-chart' "
         f"width='{CHART_W}' height='{height:.0f}' "
         f"viewBox='0 0 {CHART_W} {height:.0f}'>",
         f"<title>{title}</title>",
         CHART_STYLE,
         f"<rect width='{CHART_W}' height='{height:.0f}' class='bch-surface' "
         f"fill='{SURFACE}'/>",
         f"<text x='12' y='20' {FONT} font-size='11' class='bch-ink2' "
         f"fill='{INK2}'>&#8593; faster"
         + (" (lower mean latency)" if lower_better else "")
         + " &#183; dotted line = asio (callbacks)"
         + (f" &#183; backend = {_esc(bdisp)}" if bdisp else "")
         + "</text>"]
    lx = 12
    for c in series:
        s.append(f"<rect x='{lx}' y='{MARGIN_T}' width='10' height='10' rx='2' "
                 f"class='{series_class(c)}' fill='{series_color(c)}'/>")
        label = "corosio" if c.startswith("corosio") else "asio (coroutines)"
        s.append(f"<text x='{lx + 14}' y='{MARGIN_T + 9}' {FONT} font-size='11' "
                 f"class='bch-ink2' fill='{INK2}'>{_esc(label)}</text>")
        lx += 14 + 7 * len(label) + 18
    # horizontal gridlines and percent labels at each tick (0 gets the
    # dotted baseline below instead of a plain gridline)
    gv = lo
    while gv <= hi + 1e-9:
        if abs(gv) > 1e-9:
            gy = y(gv)
            s.append(f"<line x1='{plot_left}' y1='{gy:.1f}' "
                     f"x2='{plot_right}' y2='{gy:.1f}' class='bch-grid' "
                     f"stroke='{GRID}' stroke-width='1'/>")
            s.append(f"<text x='{plot_left - 6}' y='{gy + 3:.1f}' {FONT} "
                     f"font-size='10' class='bch-ink2' fill='{INK2}' "
                     f"text-anchor='end'>{gv:+g}%</text>")
        gv += step
    s.append(f"<text x='{plot_left - 6}' y='{y0 + 3:.1f}' {FONT} "
             f"font-size='10' class='bch-ink2' fill='{INK2}' "
             f"text-anchor='end'>0%</text>")

    for i, (name, bars) in enumerate(rows):
        gx = plot_left + i * (gw + VGROUP_GAP)
        desc = descs.get(name)
        cx = gx + group_pad
        for c, v in bars:
            yv = y(v)
            floor = top + VPLOT_H
            rect = (f"<rect x='{cx:.1f}' y='{yv:.1f}' width='{col_w:.1f}' "
                    f"height='{max(floor - yv, 1):.1f}' rx='2' "
                    f"class='{series_class(c)}' fill='{series_color(c)}'")
            if desc:
                s.append(f"{rect}><title>{_esc(name)} — "
                         f"{_esc(desc)}</title></rect>")
            else:
                s.append(f"{rect}/>")
            if label_all or v in ext[c]:
                text = _format_pct(v)
                ty = yv - 4
                tx = _clamp_middle(cx + col_w / 2.0, text)
                s.append(f"<text x='{tx:.1f}' y='{ty:.1f}' {FONT} "
                         f"font-size='10' class='bch-ink2' fill='{INK2}' "
                         f"text-anchor='middle'>{text}</text>")
            cx += col_w + COL_GAP
        tx_, ty_ = gx + gw / 2.0, top + VPLOT_H + 14
        s.append(f"<text x='{tx_:.1f}' y='{ty_:.1f}' {FONT} font-size='10' "
                 f"class='bch-ink' fill='{INK}' text-anchor='end' "
                 f"transform='rotate(-38 {tx_:.1f} {ty_:.1f})'>"
                 f"{_esc(name)}</text>")

    # The baseline goes on top of the bars: a threshold must stay
    # readable across the full width, not vanish behind whatever
    # column happens to cross it.
    s.append(f"<line x1='{plot_left}' y1='{y0:.1f}' x2='{plot_right}' "
             f"y2='{y0:.1f}' class='bch-zero' stroke='{INK2}' "
             f"stroke-width='1.5' stroke-dasharray='5 4'/>")
    s.append("</svg>")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(s) + "\n")


SEG_GAP = 2            # px gap between adjacent stacked-bar segments
SEG_LABEL_MIN_W = 16    # min segment width (px) to show its count label
MARGIN_R = 70           # px reserved right of the bar for the median annotation


def render_summary_chart(platform, summary, out_path, backend=None):
    """Write one platform's diverging stacked-bar summary SVG.

    One row per category (sorted by descending median rel for this
    config), each a single bar splitting its benchmark count into slower/
    within-noise/faster segments that straddle a shared center line. All
    rows share one px-per-benchmark scale (set by the category with the
    most benchmarks) so segment lengths are comparable down the page.
    Skipped (no file) when there's nothing to summarize.

    `backend` is the display name (e.g. "epoll") for a multi-backend
    platform's per-backend chart; omitted for a single-backend platform's
    one chart, which needs no backend qualifier in its title.
    """
    if not summary:
        return
    rows = sorted(summary.items(), key=lambda kv: kv[1]["median"], reverse=True)

    plot_w = CHART_W - LABEL_W - 20
    x0 = LABEL_W + plot_w / 2.0          # center line
    half_w = plot_w / 2.0
    # Layout diverges from the CENTER, so each side only ever has half_w
    # to work with, not the full plot_w a one-sided scale would assume;
    # sizing against max_total (as if bars started at x=0) let the widest
    # one-sided bar run off the right edge. max_extent is that widest
    # single-side reach (neutral counts split in half since they straddle
    # center); MARGIN_R reserves room for the median annotation that sits
    # to the right of the bar's actual right edge.
    max_extent = max(max(c["slower"] + c["within"] / 2.0,
                         c["faster"] + c["within"] / 2.0) for _, c in rows)
    px = (half_w - MARGIN_R) / max_extent if max_extent else 0.0

    plot_h = len(rows) * BAR_H + GROUP_GAP * (len(rows) - 1)
    height = MARGIN_T + plot_h + MARGIN_B

    # See render_chart's comment: the visible title moved to an HTML
    # heading on the page, and this <title> — the SVG's own accessible
    # name — must be the first child of <svg>, before <style>.
    plat = _esc(platform_display(platform))
    title = f"Summary ({backend}) — {plat}" if backend else f"Summary — {plat}"
    s = [f"<svg xmlns='http://www.w3.org/2000/svg' class='bch-chart' "
         f"width='{CHART_W}' height='{height:.0f}' "
         f"viewBox='0 0 {CHART_W} {height:.0f}'>",
         f"<title>{title}</title>",
         CHART_STYLE,
         f"<rect width='{CHART_W}' height='{height:.0f}' class='bch-surface' "
         f"fill='{SURFACE}'/>",
         f"<text x='12' y='20' {FONT} font-size='11' class='bch-ink2' "
         f"fill='{INK2}'>&#8592; slower &#183; within noise &#183; faster "
         f"&#8594; per benchmark, vs Boost.Asio (callbacks)</text>"]

    top = MARGIN_T
    s.append(f"<line x1='{x0:.1f}' y1='{top}' x2='{x0:.1f}' "
             f"y2='{top + plot_h:.0f}' class='bch-zero' stroke='{INK2}' "
             f"stroke-width='1.5'/>")

    y = top
    for category, cat in rows:
        s.append(f"<text x='{LABEL_W - 8}' y='{y + BAR_H / 2 + 4:.1f}' {FONT} "
                 f"font-size='11' class='bch-ink' fill='{INK}' "
                 f"text-anchor='end'>{_esc(category)}</text>")

        # Neutral straddles center; slower/faster extend outward from its
        # edges, each offset by SEG_GAP. This falls out naturally even
        # when within=0: the two gaps collapse to one empty slot at center.
        w_neg, w_mid, w_pos = (cat["slower"] * px, cat["within"] * px,
                               cat["faster"] * px)
        mid_l, mid_r = x0 - w_mid / 2.0, x0 + w_mid / 2.0
        neg_r = mid_l - SEG_GAP
        neg_l = neg_r - w_neg
        pos_l = mid_r + SEG_GAP
        pos_r = pos_l + w_pos

        for x1_, x2_, cls, fill, count, label_cls, label_fill, verdict in (
            (neg_l, neg_r, "bch-div-neg", DIV_NEG, cat["slower"],
             "bch-seg-label", "#ffffff", "slower"),
            (mid_l, mid_r, "bch-div-mid", DIV_MID, cat["within"],
             "bch-ink", INK, "within noise"),
            (pos_l, pos_r, "bch-div-pos", DIV_POS, cat["faster"],
             "bch-seg-label", "#ffffff", "faster"),
        ):
            w = x2_ - x1_
            if w <= 0:
                continue
            title = (f"{count} benchmark{'s' if count != 1 else ''} "
                    f"{verdict} than asio (callbacks)")
            s.append(f"<rect x='{x1_:.1f}' y='{y:.1f}' width='{w:.1f}' "
                     f"height='{BAR_H}' rx='2' class='{cls}' fill='{fill}'>"
                     f"<title>{_esc(title)}</title></rect>")
            if w >= SEG_LABEL_MIN_W:
                cx = _clamp_middle((x1_ + x2_) / 2.0, str(count))
                s.append(f"<text x='{cx:.1f}' y='{y + BAR_H - 2:.1f}' {FONT} "
                         f"font-size='10' text-anchor='middle' "
                         f"class='{label_cls}' fill='{label_fill}'>"
                         f"{count}</text>")

        median_text = _format_pct1(cat["median"])
        median_x = _clamp_start(pos_r + 4, median_text)
        s.append(f"<text x='{median_x:.1f}' y='{y + BAR_H - 2:.1f}' {FONT} "
                 f"font-size='10' class='bch-ink2' fill='{INK2}' "
                 f"text-anchor='start'>{median_text}</text>")
        y += BAR_H + GROUP_GAP

    s.append("</svg>")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(s) + "\n")


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


def load_platform(platform_dir):
    """Load one platform's raw runs, descriptions, and environment.json.

    Descriptions are corosio-only (the asio-side binaries don't emit
    them): per-benchmark `"description"` strings and a top-level
    `"category_descriptions"` object, both optional. The numeric-only
    filter below that builds each benchmark's metric table would silently
    drop the string description field, so it's captured on the side
    instead, first-non-empty-wins across whichever configs/iterations
    have it — returned as a third element rather than folded into `runs`,
    since it's page-rendering metadata, not a metric.
    """
    runs, env = {}, None
    bench_descs, cat_descs = {}, {}
    p_env = Path(platform_dir) / "environment.json"
    if p_env.exists():
        try:
            env = json.loads(p_env.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"warning: unreadable environment.json: {e}", file=sys.stderr)
    for p in sorted(Path(platform_dir).iterdir()):
        m = FNAME.match(p.name)
        if not m:
            continue
        config, it = m.group("config"), int(m.group("iter"))
        try:
            payload = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"warning: skipping unreadable {p.name}: {e}", file=sys.stderr)
            continue
        if not isinstance(payload, dict):
            print(f"warning: skipping non-object payload {p.name}", file=sys.stderr)
            continue
        benches = payload.get("benchmarks", [])
        if not isinstance(benches, list):
            print(f"warning: skipping non-list benchmarks in {p.name}", file=sys.stderr)
            continue
        cds = payload.get("category_descriptions")
        if isinstance(cds, dict):
            for cat, desc in cds.items():
                if isinstance(desc, str) and desc and cat not in cat_descs:
                    cat_descs[cat] = desc
        table = {}
        for b in benches:
            if not isinstance(b, dict):
                print(f"warning: skipping non-object entry in {p.name}", file=sys.stderr)
                continue
            key = (b.get("category", ""), b.get("name", ""))
            table[key] = {k: v for k, v in b.items()
                          if isinstance(v, (int, float)) and not isinstance(v, bool)}
            desc = b.get("description")
            if isinstance(desc, str) and desc and key not in bench_descs:
                bench_descs[key] = desc
        runs.setdefault(config, {})[it] = table
    return runs, env, {"benchmarks": bench_descs, "categories": cat_descs}


def _cv(vals):
    if len(vals) < 2:
        return None
    mean = statistics.fmean(vals)
    if mean == 0:
        return None
    return statistics.stdev(vals) / abs(mean) * 100.0


def aggregate(runs):
    """Median + CV of the primary metric per benchmark per configuration."""
    keys, sample = set(), {}
    for config, iters in runs.items():
        for table in iters.values():
            keys.update(table)
            for k, metrics in table.items():
                sample.setdefault(k, metrics)
    agg = {}
    for key in sorted(keys):
        category, _ = key
        metric, direction = primary_metric(category, sample.get(key, {}))
        if metric is None:
            continue
        configs = {}
        for config, iters in runs.items():
            vals = [t[key][metric] for _, t in sorted(iters.items())
                    if key in t and metric in t[key]]
            if not vals:
                continue
            configs[config] = {"median": statistics.median(vals),
                               "cv": _cv(vals), "n": len(vals)}
        if configs:
            agg[key] = {"metric": metric, "direction": direction,
                        "configs": configs}
    return agg


def baseline_for(config, present_configs):
    """Return the asio baseline config matched to `config`, or None if
    `config` IS itself a baseline (gets no rel of its own).

    On Linux, asio is built once per reactor (epoll, io_uring), so a
    `corosio-<reactor>` or `asio_callback-<reactor>` config compares
    against the SAME-reactor asio build (`asio-<reactor>`) when one is
    present, rather than a single reactor-blind `asio`. Configs with no
    reactor suffix — single-reactor platforms, or a dataset that only
    ever shipped plain `asio`/`asio_callback` — fall back to plain
    `asio`, exactly as before this reactor-matching was added.
    """
    if config == BASELINE or config.startswith("asio_callback-"):
        return None
    if "-" in config:
        prefix, _, flavor = config.rpartition("-")
        if prefix in ("corosio", "asio"):
            matched = f"asio_callback-{flavor}"
            if matched in present_configs:
                return matched
    return BASELINE


def _configs_seen(agg):
    """All configs with at least one benchmark's data anywhere in `agg`."""
    return {c for row in agg.values() for c in row["configs"]}


def corosio_configs_present(agg, series):
    """Which of `series`'s corosio-* configs actually have data in `agg`.

    `series` (PLATFORM_SERIES[platform]) is a static per-platform list;
    on Linux it names both reactors even when a given run only actually
    captured one of them (e.g. an older-shaped dataset, or a partial
    artifact), so "is this platform multi-backend" must be answered from
    the real data, not the static list.
    """
    seen = _configs_seen(agg)
    return [c for c in series if c.startswith("corosio") and c in seen]


def relativize(agg):
    """Add sign-normalized percent-vs-matched-baseline for every config
    that has one (see `baseline_for`); baseline configs get no rel."""
    for row in agg.values():
        present = row["configs"]
        rel = {}
        for config, c in present.items():
            baseline_key = baseline_for(config, present)
            if baseline_key is None:
                continue
            base = present.get(baseline_key)
            if not base or base["median"] <= 0:
                continue
            r = (c["median"] / base["median"] - 1.0) * 100.0
            if row["direction"] == "lower":
                r = -r
            rel[config] = r
        if rel:
            row["rel"] = rel
    return agg


def _ratio_noise(cv_a, cv_b):
    parts = [c for c in (cv_a, cv_b) if c is not None]
    if not parts:
        return None
    return math.hypot(*parts) if len(parts) == 2 else parts[0]


def _config_rels(agg, config):
    """Yield (category, name, rel_value, noise) for one config's matched-
    baseline comparisons (see `baseline_for`); benchmarks where `config`
    has no rel (no baseline, or `config` absent) are skipped. Shared by
    `summarize()` (per-category breakdown) and the platform page's
    per-backend stat line (flattened across all categories).
    """
    for (category, name), row in agg.items():
        rel = row.get("rel")
        if not rel or config not in rel:
            continue
        # Noise must pair against the SAME baseline `config` was
        # relativized against, not a fixed global 'asio' CV, or a
        # reactor-matched comparison would silently mix noise from an
        # unrelated build.
        baseline_key = baseline_for(config, row["configs"])
        base_cv = (row["configs"][baseline_key]["cv"]
                  if baseline_key in row["configs"] else None)
        noise = _ratio_noise(row["configs"][config]["cv"], base_cv)
        yield category, name, rel[config], noise


def summarize(agg, config):
    """Per-category verdict counts for ONE corosio config vs its own
    matched baseline (not an aggregate across every corosio backend —
    each backend gets its own independent summary on the platform page).
    """
    out = {}
    vals = {}
    for category, name, val, noise in _config_rels(agg, config):
        cat = out.setdefault(category, {"faster": 0, "within": 0, "slower": 0})
        vals.setdefault(category, []).append(val)
        if noise is not None and abs(val) <= 2.0 * noise:
            cat["within"] += 1
        elif val > 0:
            cat["faster"] += 1
        else:
            cat["slower"] += 1
    for category, cat in out.items():
        cat["median"] = statistics.median(vals[category])
    return out


ENV_FIELDS = [("cpu", "CPU"), ("cores", "Cores"), ("ram_gb", "RAM (GB)"),
              ("os", "OS"), ("kernel", "Kernel/build"), ("compiler", "Compiler"),
              ("cmake", "CMake"), ("liburing", "liburing"),
              ("boost_sha", "Boost commit"), ("asio_sha", "Asio commit"),
              ("asio_reactor", "Asio reactor"),
              ("capy_sha", "Capy commit"), ("corosio_sha", "Corosio commit"),
              ("corosio_ref", "Corosio branch"),
              ("date_utc", "Date (UTC)"), ("iterations", "Iterations"),
              ("duration_s", "Duration per benchmark (s)")]


def human(value, metric):
    """Format a metric value with readable units.

    Copied verbatim from `compare.py`'s `human()`; see this module's
    docstring for why the metric-formatting logic is duplicated instead
    of shared.
    """
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


def _table_rows(rows, series, names=None, configs=None):
    """Render one AsciiDoc table row per (benchmark, configuration).

    Row order: the charted `series` first, then any other non-baseline
    config present, then the baseline config(s) last. `names` restricts
    the benchmarks (one chart's worth) and `configs` the configurations
    (one backend's slice); both default to everything in `rows`.
    """
    lines = []
    for name in (sorted(rows) if names is None else names):
        row = rows[name]
        rel = row.get("rel") or {}
        present = row["configs"]
        baselines = sorted(c for c in present if baseline_for(c, present) is None)
        others = sorted(c for c in present
                        if c not in series and c not in baselines)
        for config in (*series, *others, *baselines):
            c = present.get(config)
            if c is None or (configs is not None and config not in configs):
                continue
            cv = "—" if c["cv"] is None else f"{c['cv']:.2f}%"
            if config in baselines:
                # A flavored baseline (asio-epoll/asio-uring) notes which
                # reactor it was built against; the single reactor-blind
                # 'asio' baseline needs no qualifier.
                if config.startswith("asio_callback-"):
                    flavor = config[len("asio_callback-"):]
                    flavor = "io_uring" if flavor == "uring" else flavor
                    vs = f"baseline ({flavor})"
                else:
                    vs = "baseline"
            else:
                r = rel.get(config)
                vs = "no asio equivalent" if r is None else f"{r:+.1f}%"
            lines.append(f"| `{name}` | {SERIES_LABEL.get(config, config)} "
                         f"| {human(c['median'], row['metric'])} | {cv} | {vs}")
    return lines


def _suite_size_sentence(platforms):
    """Summarize benchmark/category counts per platform, from agg/by_cat."""
    clauses = []
    for platform, data in platforms.items():
        if data is None:
            continue
        n, c = len(data["agg"]), len(data["by_cat"])
        clauses.append(f"{n} benchmark{'s' if n != 1 else ''} across "
                        f"{c} categor{'ies' if c != 1 else 'y'} on "
                        f"{platform_display(platform)}")
    if not clauses:
        return None
    return "This run measured " + "; ".join(clauses) + "."


def _generated_stamp(meta, note):
    """Build the '_Generated ...' byline, omitting the run link when absent."""
    if meta["run_url"]:
        source = (f" from corosio `{meta['corosio_sha'][:12]}` — "
                  f"{meta['run_url']}[benchmark run].")
    else:
        source = f" from corosio `{meta['corosio_sha'][:12]}`."
    return (f"_Generated {meta['date']}{source} This page is fully "
            f"generated; do not edit by hand (see {note})._")


def build_index_page(platforms, meta):
    """Build the Benchmarks landing page: methodology + links to platforms.

    `:page-aliases:` keeps the old single-page URL resolving here, since
    this replaces `benchmark-report.adoc`.
    """
    L = []
    a = L.append
    a("= Boost.Corosio Performance Benchmarks")
    a(":page-aliases: benchmark-report.adoc")
    a(":page-mode: explanation")
    a(":toc: left")
    a("")
    a(_generated_stamp(meta, "<<Reproducing>>"))
    a("")
    a("This report compares Boost.Corosio against Boost.Asio using "
      "coroutines (the `asio` configuration) and Boost.Asio using "
      "callback-based handlers (`asio_callback`) across the corosio "
      "benchmark suite. Its purpose is to track corosio's performance "
      "relative to an established async I/O library as both evolve, not "
      "to crown a universal winner.")
    a("")
    a("== Methodology")
    a("")
    suite_sentence = _suite_size_sentence(platforms)
    if suite_sentence:
        a(suite_sentence)
        a("")
    a("Each benchmark runs several iterations of a fixed duration, "
      "interleaved across configurations. Drift from thermal throttling "
      "or background load therefore lands on every configuration alike, "
      "rather than skewing one of them. The reported figure for a "
      "benchmark/configuration pair is the median across its iterations, "
      "which resists a single outlier iteration. The coefficient of "
      "variation (CV) across those iterations appears alongside it as a "
      "measure of run-to-run noise. See each platform page's Test "
      "Environment section for the exact iteration count and duration "
      "used there.")
    a("")
    a("Metric selection and the higher/lower-is-better direction follow "
      "the same rules as the PR comparison bot "
      "(`.github/bench/compare.py`). Latency categories compare "
      "`latency_mean_ns` (lower is better); everything else compares "
      "`bytes_per_sec`, then `items_per_sec`, then `ops_per_sec` (higher "
      "is better). Percentages on this page are sign-normalized so that "
      "a positive value always means corosio performed better than "
      "asio.")
    a("")
    a("Every percentage on these pages is measured against Boost.Asio "
      "with callback handlers on the same reactor. Boost.Asio's own "
      "coroutine frontend (the `asio` configuration) is plotted "
      "alongside corosio against that same baseline.")
    a("")
    a("On Linux, asio is built twice: once against the epoll reactor, "
      "once against io_uring. Every comparison pairs implementations on "
      "the same reactor, never judging an io_uring corosio backend "
      "against an epoll-only asio. On Windows and macOS, asio uses its "
      "native reactor (IOCP and kqueue respectively), matching corosio's "
      "own default backend there. Each platform page records the reactor "
      "used on that machine.")
    a("")
    a("Each platform's numbers come from a dedicated, otherwise-idle "
      "machine, to keep competing workloads from adding noise to the "
      "measurements.")
    a("")
    a("== Reproducing")
    a("")
    a("The full suite layout and exact commands are documented in "
      "`bench/README.md`. In short: dispatch the benchmark-report "
      "workflow, download its artifacts, and regenerate this page with:")
    a("")
    a("[source,bash,role=external]")
    a("----")
    a("python3 .github/bench/report_page.py --input-dir <artifacts-dir> \\")
    a("    --output-dir doc/modules/ROOT")
    a("----")
    a("")
    if meta["run_url"]:
        a(f"Source data: {meta['run_url']}[workflow run]. Regenerate this "
          f"page with the command above. GitHub retains the run's raw "
          f"JSON artifacts for 90 days; these pages are the durable "
          f"record.")
    else:
        a("Regenerate this page with the command above. GitHub retains a "
          "workflow run's raw JSON artifacts for 90 days; these pages "
          "are the durable record.")
    a("")
    a("== Platforms")
    a("")
    for platform in platforms:
        a(f"* xref:benchmarks/{platform}.adoc[{platform_display(platform)}]")
    return "\n".join(L) + "\n"


def _chart_parts(svg_names, base):
    """The actual chart file(s) on disk for `base`: the single
    `<base>.svg`, or the `-g1`... run a split category produced."""
    if f"{base}.svg" in svg_names:
        return [f"{base}.svg"]
    out, i = [], 1
    while f"{base}-g{i}.svg" in svg_names:
        out.append(f"{base}-g{i}.svg")
        i += 1
    return out


def build_platform_page(platform, data, generated, meta):
    """Build one platform's page: summary, test environment, results."""
    L = []
    a = L.append
    a(f"= {platform_display(platform)} Benchmarks")
    a(":page-mode: explanation")
    a(":toc: left")
    a("")
    a(_generated_stamp(
        meta, "the xref:benchmarks/index.adoc[Benchmarks landing page]"))
    a("")
    if data is None:
        a(f"No results for {platform} in this run.")
        return "\n".join(L) + "\n"
    a("== Summary")
    a("")
    a("_Within noise_ means the relative difference is within twice the "
      "combined run-to-run noise (the root-sum-square of each side's "
      "CV). Differences that small are indistinguishable from "
      "measurement jitter. See xref:benchmarks/index.adoc[Methodology] "
      "for how these figures are computed.")
    a("")
    series = PLATFORM_SERIES.get(platform, [])
    corosio_configs = corosio_configs_present(data["agg"], series)
    multi_backend = len(corosio_configs) > 1
    summaries = data.get("summaries") or {}
    for config in corosio_configs:
        summary = summaries.get(config)
        if not summary:
            continue
        if multi_backend:
            a(f"=== {backend_display(config)}")
            a("")
        faster = sum(cat["faster"] for cat in summary.values())
        within = sum(cat["within"] for cat in summary.values())
        slower = sum(cat["slower"] for cat in summary.values())
        total = faster + within + slower
        overall = statistics.median(
            [v for _, _, v, _ in _config_rels(data["agg"], config)])
        a(f"**{faster} faster &#183; {within} within noise &#183; "
          f"{slower} slower** of {total} "
          f"benchmark{'s' if total != 1 else ''} &#8212; median "
          f"**{_format_pct1(overall)}** vs Boost.Asio (callbacks) on the "
          f"same reactor.")
        a("")
        suffix = config[len("corosio-"):]
        fname = (f"{platform}-summary-{suffix}.svg" if multi_backend
                 else f"{platform}-summary.svg")
        a(f"image::bench/{fname}[summary,role=bch-block,opts=inline]")
        a("")
    a("== Test Environment")
    a("")
    env = data["env"] or {}
    a('[cols="1,3"]')
    a("|===")
    for key, label in ENV_FIELDS:
        val = env.get(key, "unknown")
        # Literal tool output reads as code, and spares the spell checker
        if key == "cmake":
            val = f"`{val}`"
        a(f"| {label} | {val}")
    a("|===")
    a("")
    a("== Results")
    a("")
    svg_names = {p.name for p in generated}
    series = PLATFORM_SERIES.get(platform, [])
    # The chart's own visible title moved out of the SVG (now just an
    # accessible-name <title>, not rendered), so the heading is the only
    # place the category name shows — every category gets one, including
    # a chartless one (no benchmark in it has an asio baseline to compare
    # against), which now reads identically to the rest apart from the
    # missing image.
    descs = data.get("descs") or {"benchmarks": {}, "categories": {}}
    categories = sorted(data["by_cat"].items())
    for i, (category, rows) in enumerate(categories):
        a(f"=== `{category}`")
        a("")
        cat_desc = descs["categories"].get(category)
        if cat_desc:
            # Plain AsciiDoc text, not XML/HTML-escaped: _esc() is for the
            # SVG <title> elements below, and would corrupt a literal '&'
            # in hand-authored description text into a visible "&amp;".
            a(cat_desc)
            a("")
        all_configs = {c for r in rows.values() for c in r["configs"]}
        covered = set()
        def emit_table(part_names, configs):
            a(".Detailed results")
            # bch-exact role lets the chart's CSS patch this
            # collapsible's summary label color, which the theme's own
            # dark rules miss
            a("[%collapsible.bch-exact]")
            a("====")
            a('[%autowidth.stretch,options="header"]')
            a("|===")
            a("| Benchmark | Implementation | Median | CV | vs asio")
            L.extend(_table_rows(rows, series, part_names, configs))
            a("|===")
            a("====")
            a("")
        if multi_backend:
            # One chart per corosio backend, each isolating that
            # backend's own bars (and its matched asio flavor) so epoll
            # and io_uring numbers are never read off the same plot; a
            # bold label stands in for a heading since these are
            # siblings within one category, not sections of their own.
            # Each chart carries its own collapsible with exactly the
            # benchmarks and configurations it plots (plus their
            # baseline).
            for config in corosio_configs:
                suffix = config[len("corosio-"):]
                fnames = _chart_parts(
                    svg_names, f"{platform}-{category}-{suffix}")
                if not fnames:
                    continue
                bseries = [config, f"asio-{suffix}"]
                bconfigs = set(bseries) | {f"asio_callback-{suffix}"}
                part_lists = split_benchmarks(
                    [n for n in sorted(rows)
                     if (rows[n].get("rel") or {}).keys() & set(bseries)])
                if part_lists:
                    covered.update(*part_lists)
                label = backend_display(config)
                a(f"**{label}**")
                a("")
                for part, (fname, pnames) in enumerate(
                        zip(fnames, part_lists), 1):
                    alt = f"{category.replace('_', ' ')} comparison ({label})"
                    if len(fnames) > 1:
                        alt += f", part {part}"
                    a("[.bch-card]")
                    a("--")
                    a(f"image::bench/{fname}[{alt},role=bch-block,"
                      f"opts=inline]")
                    a("")
                    emit_table(pnames, bconfigs)
                    a("--")
                    a("")
        else:
            # inline (not <img>) so the SVG's <style> can see the docs
            # site's html.dark class and follow its theme toggle; no
            # fixed width here — the chart's own CSS makes it fluid.
            # role=bch-block tags the wrapping <div class="imageblock">
            # so the chart's CSS can stretch just this block, not every
            # imageblock on the page.
            fnames = _chart_parts(svg_names, f"{platform}-{category}")
            part_lists = split_benchmarks(
                [n for n in sorted(rows) if rows[n].get("rel")])
            if part_lists:
                covered.update(*part_lists)
            for part, (fname, pnames) in enumerate(
                    zip(fnames, part_lists), 1):
                alt = f"{category.replace('_', ' ')} comparison"
                if len(fnames) > 1:
                    alt += f", part {part}"
                a("[.bch-card]")
                a("--")
                a(f"image::bench/{fname}[{alt},role=bch-block,"
                  f"opts=inline]")
                a("")
                emit_table(pnames, None)
                a("--")
                a("")
        # Benchmarks no chart plots (nothing to compare against the
        # baseline) still get their exact values recorded.
        leftover = [n for n in sorted(rows) if n not in covered]
        if leftover:
            a("[.bch-card]")
            a("--")
            emit_table(leftover, None)
            a("--")
            a("")
        if i < len(categories) - 1:
            # Plain vertical space between categories, not a rule: <hr>
            # (what a thematic break '''  becomes) renders badly in dark
            # mode, so a passthrough <br/> stands in for it instead.
            a("++++")
            a("<br/>")
            a("++++")
            a("")
    return "\n".join(L) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(prog="report_page.py")
    ap.add_argument("--input-dir", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--expect", default="linux,windows,macos")
    args = ap.parse_args(argv)
    platforms = {}
    for platform in args.expect.split(","):
        d = Path(args.input_dir) / f"bench-report-{platform}"
        platforms[platform] = load_platform(d) if d.is_dir() else None
        if platforms[platform] is None:
            print(f"warning: no artifact for {platform}", file=sys.stderr)
    generated = write_outputs(platforms, Path(args.output_dir))
    print(f"wrote {len(generated['pages'])} pages and "
          f"{len(generated['svgs'])} charts")
    return 0


def write_outputs(platforms, output_dir):
    pages = Path(output_dir) / "pages" / "benchmarks"
    images = Path(output_dir) / "images" / "bench"
    pages.mkdir(parents=True, exist_ok=True); images.mkdir(parents=True, exist_ok=True)
    svgs = []
    prepared = {}
    meta = {"date": "unknown", "corosio_sha": "unknown", "run_url": ""}
    seen_env = False
    for platform, loaded in platforms.items():
        if loaded is None:
            prepared[platform] = None
            continue
        runs, env, descs = loaded
        if env and not seen_env:
            meta = {"date": env.get("date_utc", "unknown"),
                    "corosio_sha": env.get("corosio_sha", "unknown"),
                    "run_url": env.get("run_url", "")}
            seen_env = True
        agg = relativize(aggregate(runs))
        series = PLATFORM_SERIES.get(platform, [])
        # Known once per platform, ahead of both the per-category charts
        # and the per-backend summaries below: which corosio backends
        # actually have data (not just which PLATFORM_SERIES names), and
        # every config with any data at all (used to decide whether a
        # backend's matched asio_callback flavor has anything to chart).
        corosio_configs = corosio_configs_present(agg, series)
        multi_backend = len(corosio_configs) > 1
        seen_configs = _configs_seen(agg)
        by_cat = {}
        for (category, name), row in agg.items():
            by_cat.setdefault(category, {})[name] = row
        for category, rows in sorted(by_cat.items()):
            cat_bench_descs = {n: descs["benchmarks"][(category, n)]
                               for n in rows
                               if (category, n) in descs["benchmarks"]}
            if multi_backend:
                # One chart per corosio backend rather than one shared
                # chart: each backend's plot carries only its own bars
                # plus its matched asio_callback flavor (omitted if that
                # flavor has no data at all), so epoll and io_uring never
                # share an axis or a legend.
                for config in corosio_configs:
                    suffix = config[len("corosio-"):]
                    asio_cfg = f"asio-{suffix}"
                    backend_series = [config] + (
                        [asio_cfg] if asio_cfg in seen_configs else [])
                    chartable = {n: r for n, r in rows.items()
                                if r.get("rel") and
                                any(c in r["rel"] for c in backend_series)}
                    svgs.extend(render_chart(
                        platform, category, chartable, backend_series,
                        images / f"{platform}-{category}-{suffix}",
                        cat_bench_descs,
                        backend=backend_display(config)))
            else:
                chartable = {n: r for n, r in rows.items() if r.get("rel")}
                svgs.extend(render_chart(
                    platform, category, chartable, series,
                    images / f"{platform}-{category}", cat_bench_descs))
        # One independent summary (counts + chart) per corosio backend —
        # on Linux that's epoll and io_uring separately, not a single
        # "best of the two" aggregate; single-backend platforms still get
        # exactly one, just without a backend-suffixed filename.
        summaries = {}
        for config in corosio_configs:
            summary = summarize(agg, config)
            suffix = config[len("corosio-"):]
            fname = (f"{platform}-summary-{suffix}.svg" if multi_backend
                     else f"{platform}-summary.svg")
            out = images / fname
            out.unlink(missing_ok=True)   # same stale-resurrection guard as above
            render_summary_chart(platform, summary, out,
                                backend=backend_display(config)
                                if multi_backend else None)
            if out.exists():
                svgs.append(out)
            summaries[config] = summary
        prepared[platform] = {"env": env, "agg": agg, "by_cat": by_cat,
                              "summaries": summaries, "descs": descs}
    for stale in images.glob("*.svg"):
        if stale not in svgs:
            stale.unlink()

    out_pages = [pages / "index.adoc"]
    out_pages[0].write_text(build_index_page(prepared, meta))
    for platform, data in prepared.items():
        page = pages / f"{platform}.adoc"
        page.write_text(build_platform_page(platform, data, svgs, meta))
        out_pages.append(page)

    emitted_names = {p.name for p in out_pages}
    for stale in pages.glob("*.adoc"):
        if stale.name not in emitted_names:
            stale.unlink()

    return {"pages": out_pages, "svgs": svgs}


if __name__ == "__main__":
    sys.exit(main())
