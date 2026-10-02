# Benchmarks

## What lives here

- `corosio/` — corosio benchmark suites (one `.cpp` per category).
- `asio/callback/`, `asio/coroutine/` — equivalent suites against
  Boost.Asio, built only when `Boost::asio` is available at configure time.
- `common/` — shared harness: benchmark registration, timing, backend
  selection, HTTP parsing helpers.

All suites build into a single `corosio_bench` binary, selected at
runtime via `--library`. Categories: `io_context`, `socket_throughput`,
`socket_latency`, `http_server`, `accept_churn`, `fan_out`,
`local_socket_throughput`, `local_socket_latency` (the last two are POSIX
only). Run `corosio_bench --list` for the exact set of benchmarks in each
category on your build — some are gated by backend or platform.

## Building

Build inside the Boost superproject with `asio` included, so
`bench/CMakeLists.txt` picks up `Boost::asio` as a sibling target and
compiles the comparison suites:

```bash
cmake -S boost-root -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBOOST_INCLUDE_LIBRARIES="corosio;asio" \
    -DBOOST_COROSIO_BUILD_BENCH=ON
cmake --build build --config Release --target corosio_bench --parallel
```

Building corosio standalone also works
(`-DBOOST_COROSIO_BUILD_BENCH=ON` from the corosio checkout), but then
the comparison suites need a system-installed Boost 1.84+ with Asio —
`bench/CMakeLists.txt` falls back to `find_package(Boost COMPONENTS
asio)` when no sibling target exists. Either way, if configure prints:

```
Boost.Asio not found -- comparison benchmarks disabled
```

then `corosio_bench` only accepts `--library corosio`; `asio` and
`asio_callback` are compiled out entirely, not just unavailable at
runtime.

Always build `Release`. Debug disables inlining and optimization, which
distorts relative costs between fast and slow paths.

## Running locally

The binary lands at `build/bench/<config>/corosio_bench` (or
`build/bench/corosio_bench` for a single-config generator). Run
benchmarks one process at a time — never in parallel — since concurrent
processes contend for CPU and cache and produce unreliable numbers.

Full suite, default library (corosio), platform-default backend:

```bash
build/bench/Release/corosio_bench
```

One category:

```bash
build/bench/Release/corosio_bench --category socket_throughput
```

One benchmark (`--bench` is a prefix match on the benchmark name, not
the category; combine with `--category` to disambiguate across
categories):

```bash
build/bench/Release/corosio_bench --category socket_throughput \
    --bench unidirectional
```

Select a backend, comparison library, duration, and warmup:

```bash
build/bench/Release/corosio_bench --library asio_callback --backend epoll \
    --duration 5 --warmup 0.5
```

`--duration` sets the measured time per benchmark (default 3s);
`--warmup` runs an unmeasured pass first (default 0, disabled) — use it
for benchmarks sensitive to cold caches or lazy connection setup.
`--library all` runs corosio and both asio variants back to back. Write
results to JSON for later aggregation:

```bash
build/bench/Release/corosio_bench --output results/corosio-epoll-1.json
```

To compare two working trees (e.g. before/after a change), build both,
then run each several times with the same flags, alternating which one
goes first each iteration — this cancels out drift from thermal
throttling or background load instead of it favoring whichever side ran
first.

## The PR benchmark workflow

`.github/workflows/benchmarks.yml` runs `corosio_bench` on dedicated
self-hosted runners for pull requests opened by the repo owner or a
collaborator, or on any PR labeled `benchmark`. It builds base and head,
runs both interleaved (ABBA order) across several iterations per
platform, and posts a single updating PR comment
(`.github/bench/compare.py`) summarizing per-category deltas with a
within-noise/faster/slower verdict. It's advisory only — see issue #343.

## Refreshing the published report

`doc/modules/ROOT/pages/benchmark-report.adoc` is a generated page —
nothing on it is hand-edited. Refresh it after a change that meaningfully
affects performance:

```bash
# 1. Run the full suite on all three platforms and collect raw JSON.
gh workflow run benchmark-report.yml --repo cppalliance/corosio --ref develop

# 2. Download the workflow's artifacts once it completes.
gh run download <run-id> --repo cppalliance/corosio -D /tmp/bench-report

# 3. Regenerate the page and its charts from the raw JSON.
python3 .github/bench/report_page.py --input-dir /tmp/bench-report \
    --output-dir doc/modules/ROOT

# 4. Preview the rendered docs site.
./doc/build_antora.sh

# 5. Commit the regenerated page and charts (nothing else).
git add doc/modules/ROOT/pages/benchmark-report.adoc doc/modules/ROOT/images/bench
git commit -m "docs: regenerate benchmark report"
```

Never regenerate the published page from a reduced/smoke run (e.g.
`iterations=1`): single-iteration data has no noise estimate, so the
summary's within-noise/faster/slower classifications are meaningless.

`report_page.py` expects `<input-dir>` to contain one
`bench-report-<platform>/` directory per platform (`linux`, `windows`,
`macos`), each holding the raw `<config>-<iter>.json` files plus an
`environment.json`. A missing platform directory is not an error — the
page notes it and generation continues with what's present.

## Methodology

Metric selection, aggregation (median across iterations, CV for noise),
and the within-noise threshold are implemented once, in
`report_page.py`, and described in full on the generated page's own
Methodology section — that page is the source of truth, not this file.
