# Documentation example verification

Two harnesses that verify the example code shipped in the documentation.

## `run_examples.py` — runtime smoke tests

Builds-and-runs the real programs under `example/` against a Python peer and
asserts on their behavior:

| Example        | Check                                                        |
|----------------|-------------------------------------------------------------|
| `nslookup`     | resolves `localhost` to `127.0.0.1` / `::1`                 |
| `echo-server`  | byte-for-byte round-trip of a payload                       |
| `hash-server`  | returns a hex digest line for sent data                    |
| `http_client`  | GETs a one-shot HTTP responder, prints the response, rc 0  |
| `reconnect`    | connects, runs a session, re-enters the retry loop         |
| `https-client` | build + usage only (a real session needs a trusted TLS peer)|

Prerequisite: the examples must be built (e.g. `cmake --build build`). They are
found under `build/example/*/Release/`.

```
python3 test/doc/run_examples.py
```

Exit code is non-zero if any runnable example fails.

## `snippet_check.py` — example-snippet compile check

Extracts every `[source,cpp]` block from the AsciiDoc pages and every `@code`
block from the public headers, then compiles each (`clang -fsyntax-only`,
C++20) against the real corosio/capy headers. For each page it prepends that
page's own documented *"Code snippets assume:"* preamble and injects a
**type-correct, per-page context** (so `sock`, `ctx`, `buffer`, `acc` get the
types that page intends — tcp vs udp socket, `io_context` vs `tls_context`, tcp
vs local acceptor). Each block is wrapped as a file-scope unit, a coroutine
body, and a function body; it passes if any wrapping compiles cleanly.

```
python3 test/doc/snippet_check.py --scope all      # docs + headers
python3 test/doc/snippet_check.py --scope docs -v  # show fragment detail
```

### Result categories

- **PASS** — compiled clean. Genuinely verified.
- **FRAGMENT** — fails only because it references identifiers defined in
  surrounding prose or an earlier block (e.g. a user-defined `my_http_get`, or
  a `worker` class shown earlier on the page). Reference-guide snippets are
  fragments by design; this is expected, not a defect.
- **ELIDED** — contains `...` placeholder pseudocode.
- **NON_CODE** — a result table or prose block that happens to be tagged
  `[source]` (e.g. the benchmark report).
- **API_FAIL** — fails with an error that names a real corosio/capy type or
  namespace (`no member named X in 'boost::corosio…'`, wrong namespace, deleted
  ctor, …). These survive missing context, so they are **genuine API bugs**.
  The harness exits non-zero iff any API_FAIL is present.

The harness deliberately does not try to force every fragment to compile in
isolation — doing so would require rewriting the guide into a doctest corpus.
Its contract is: maximize genuinely-compiled snippets, and drive `API_FAIL` to
zero so the documented API stays congruent with the headers.
