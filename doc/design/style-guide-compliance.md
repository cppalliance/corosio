# Bringing Corosio's Documentation Into Style-Guide Compliance

## 1. Introduction

**Scope**: This document specifies the work needed to make Corosio's documentation
comply with `libs/capy/doc/STYLE_GUIDE.md`, and to port Capy's CI enforcement
toolkit into Corosio so compliance is machine-checked rather than asserted.

**Two deliverables**:

1. A `doc/lint/` toolkit, a Vale configuration, and a Corosio `doc/STYLE_GUIDE.md`,
   wired into the existing Documentation workflow.
2. A phased remediation of the measured backlog, ordered so each phase is
   verifiable before the next begins.

**Non-goal**: changing Capy. Capy's own enforcement gaps are recorded in section 7
as follow-ups with no owner in this plan.

## 1a. Status

| Phase | State |
|---|---|
| 0 — infrastructure | **done** — toolkit ported, bite-tested, CI wired, baseline seeded |
| 1 — A1/E2/A6/ANCHOR/B5 | **done** — all four checks at zero |
| 2 — B2/B3 retagging | **done** — B2 at zero, SHAPE clean and proven live |
| 3 — B1 `cpp:` conversion | **done** — pages: 186 backtick spans + 30 bare names. Docstrings: 93 bare identifiers backticked and three Doxygen extractor fixes; corpus 420 → 62 |
| 4 — C11 `@pre` | **done** — no `@par Preconditions` remains |
| 5 — C9/C10 | **done** — both rules at zero on both corpora |
| 6 — C4 present simple | **done** — both rules at zero on both corpora |
| 7 — C2 sentence length | **done** — hard 131 → 1, and that one is the `@n` artifact |
| 8 — B4 briefs | **done** — 40 class briefs rewritten; scope noted at the phase |
| 9 — close the gate | **done** — CI baseline installed; selftest and every gated rule strict except `mrdocs_warnings` (see below) |

Measured after phase 9, both corpora: Vale over `doc/modules` 466 → 132;
`Vale.Spelling` 284 → 36; Vale over `lint/.docstrings` 785 → 427; `doc_lint`
A1/A6/B2/ANCHOR all 0, D2 3 (the documented carve-out); C9/C10 0/0; C4 0/0;
`sentence_length` hard 131 → 1, advisory 70.

**How phase 9 closed.** It took three reseeds, and the safety net refused two of them:
the first because the MrDocs version pin reported `mrdocs_warnings` as SKIPPED (section
4.3), the second on a real gated `Corosio.SimpleTense` regression that no local run
surfaced. The accepted candidate retires 365 fingerprints and grandfathers one
`Google.OxfordComma` false positive, documented in `doc/lint/README.md`.

**Earlier account of the close.** The `workflow_dispatch` reseed replaced the local seed with a
CI-authored baseline: 1096 fingerprints retired, none grandfathered, none gated.
`doc_lint` and `sentence_length` measured identically in both environments (3 and 71),
confirming the split-gate reasoning; `vale_adoc` differed 132 local against 66 in CI, and
`mrdocs_warnings` 460 against 352.

After the awaitable encapsulation and the special-member documentation pass,
`mrdocs_warnings` measures **8** locally — all of them the unattributed `<tt>` findings
that reach MrDocs through libstdc++ and are not fixable in Corosio — and **0** in CI,
which has never emitted them. The sixth reseed (2026-09-10T17:09Z) retired the last 44,
so the reference surface is clean and every future MrDocs warning is a new one; the
seventh (2026-09-21T17:47Z) is what is installed. See `doc/lint/README.md` for how that
zero was authorised.

Every gated rule is now strict except `mrdocs_warnings`. That one keeps reporting because
the baseline is zero, so its `.*` spec would gate every warning that ever appears, while
MrDocs is a rolling `develop-release` build whose output moved by 108 warnings on an
unchanged tree between two runs days apart. Gating a tool
that rewrites its own output is the same mistake as the version pin in 4.3. Promote it only
alongside a pinned MrDocs.

**Note for phase 7.** `sentence-length.mjs` does not treat Doxygen's `@n` as a
sentence boundary, so `io_context`'s boost-wide thread-safety idiom
("Distinct objects: Safe.@n Shared objects: Safe, unless ...") is measured as one
29-word sentence rather than two short ones. That finding is an artifact, not a long
sentence; do not "fix" the docstring for it.

### Corrections to this document

Three claims below were wrong when written and are corrected in place. They are
listed here because the plan was approved on their strength.

1. **A1 mode mapping.** Section 6 phase 1 assigned `2.networking-tutorial/*` the
   `tutorial` mode. That chapter has no `include::example$` anywhere, no imperative
   steps, and purely expository headings, so `explanation` is the accurate Diátaxis
   mode. Declaring it a tutorial would be a false declaration under A1 and would
   contradict the D2 and C2 carve-outs, which both rest on the chapter being
   background material.
2. **B1 and `Vale.Spelling`.** Section 3 said phase 3 "retires the Corosio-symbol
   share" of the 284 spelling alerts, and phase 3's exit criterion said the count
   would fall "by the corresponding amount". Both are false: a backtick span and a
   `cpp:` macro are both in `TokenIgnores`, so Vale never saw either. The 284 were a
   different B1 shape — identifiers with no code span at all — and needed their own
   work, done as a follow-on and recorded under phase 3.
3. **Per-phase gate promotion.** Section 6 said each phase should "promote that rule
   in the gate spec", which contradicts D-5. The gate spec carries every rule from
   phase 0 onward; what is deferred to phase 9 is only the `--strict` flag. There is
   no per-phase gate edit to make.

## 2. Decisions

These were settled before the plan was written. Each shapes the phasing.

| # | Decision | Rationale |
|---|---|---|
| D-1 | Port the machinery, seed `baseline.json` from the current backlog, then burn down | CI stays green from day one and no *new* violation can land while the backlog is worked. The alternative — remediate first — leaves a long stretch with no check running and every fix unguarded against regression. |
| D-2 | Copy the Vale styles into `doc/.vale`, renamed `Corosio/*` | Corosio's docs job must not depend on Capy's doc-tree layout, even though the CI already clones Capy. Divergence from Capy's copies is the accepted cost. |
| D-3 | Convert only `boost::corosio` public symbols to `cpp:` macros | `doc/mrdocs.yml` sets `include-symbols: boost::corosio::**`, so Corosio's site contains no Capy reference pages. A `cpp:boost::capy::…[]` macro would render a dead link. `capy::`/`cond::` and `std::` spans stay backticks. |
| D-4 | Remediate both corpora: pages *and* extracted header docstrings | The docstring corpus carries the larger share of the C2/C4 backlog and all of C11. Capy gates `vale_docstrings`; excluding it would leave most of the problem unmeasured. |
| D-5 | Gate runs report-only during the burn-down; `--strict` and a blocking `selftest.mjs` are the **final phase's exit criteria** | Matches Capy's stated intent on Corosio's timeline. Section 7 records the cost: until the flip, a new violation sits in the report until a reseed grandfathers it. |
| D-6 | Corosio gets its own `doc/STYLE_GUIDE.md`, retaining every rule, with Capy-specific carve-outs and evidence replaced by Corosio's | The guide is normative for Corosio's authors and agents; it must describe Corosio's corpus, not Capy's. |
| D-7 | Port `mrdocs-warnings.mjs` (with its version pin removed); do **not** port `run-a11y.mjs` | See sections 4.3 and 4.4. |

## 3. Measured current state

Produced by running Capy's own scripts against Corosio's corpus. These are the
numbers `baseline.json` will be seeded from; re-measure in CI before seeding,
because a local run drifts from a CI run.

| Rule | Finding | Pages | Docstrings |
|---|---|---|---|
| A1 | no `:page-mode:` attribute | **48 of 48** | — |
| A6 | `quick-start.adoc` is 8th of 9 top-level nav entries | 1 | — |
| A7 | numbered "Part N" headings | 0 | 0 |
| B1 | `cpp:` macros in prose | **0** | — |
| B2/B3 | raw code in an untagged block | **39** | — |
| ANCHOR | `` `[[...]]` `` renders as an empty `<code>` | 2 | — |
| B4 | identity-shaped briefs | — | ~30 |
| B5 | `using namespace boost::corosio` / `boost::capy` in doc code | — | 3 |
| C2 | sentences over 25 words | **143** | **61** |
| C4 | `Corosio.SimpleTense` + `Google.Will` | 67 | 186 |
| C9 | `Corosio.NoFluff` | 8 | 1 |
| C10 | `Corosio.Terminology` | 10 | 8 |
| C11 | `@par Preconditions` instead of `@pre` | — | **41** across 17 headers |
| E2 | `page-toc` attribute in `doc/antora.yml` | **missing** | — |
| D2 | concept page with no `include::example$` | see 4.2 | — |

`Vale.Spelling` reports 284 on pages and 552 on docstrings. Most are bare C++
identifiers used as running text, which Capy's `.vale.ini` deliberately leaves
unlisted because they are B1 defects. They are not a separate work item: phase 3
retires the Corosio-symbol share, and `accept.txt` absorbs the genuine prose
words and proper nouns.

**Already compliant**: B2 is 381 of 399 `[source]` blocks sourced from compiled
files, with `test/doc/{snippets,programs,reference}` and the `antora.yml`
collector wiring already in place. A7 is clean. A glossary exists (C7).

## 4. Infrastructure to port

### 4.1 File manifest

Created under `libs/corosio/doc/`:

```
STYLE_GUIDE.md
.vale.ini
.vale/styles/Corosio/{Terminology,NoFluff,SimpleTense,PartHeadings,SentenceLength}.yml
.vale/styles/config/vocabularies/Corosio/accept.txt
.vale/styles/Google/                      (the upstream pack, as Capy vendors it)
lint/README.md
lint/doc-lint.mjs
lint/extract-docstrings.mjs
lint/sentence-length.mjs
lint/mrdocs-warnings.mjs
lint/check-include-tags.mjs
lint/selftest.mjs
lint/baseline.mjs
lint/baseline-diff.mjs
lint/check-no-new-violations.mjs
lint/baseline.json                        (seeded in CI, not locally)
```

`package.json` gains the `asciidoctor` devDependency Vale shells out to. It does
**not** gain `pa11y-ci` (section 4.4).

### 4.2 Corosio-specific configuration — decisions, not renames

Beyond replacing `capy`→`corosio` in paths and rule names, four values carry real
judgment:

**`doc-lint.mjs`'s `CONCEPT_DIRS` (D2).** Corosio's chapters are
`2.networking-tutorial`, `3.tutorials`, `4.guide`, `5.testing`.
`2.networking-tutorial` teaches IP, TCP, and UDP theory and introduces no Corosio
type — structurally the same case as Capy's `3a`–`3d` primer. It gets the same
explicit carve-out, recorded in Corosio's `STYLE_GUIDE.md` at the D2 entry.
Without it D2 fires roughly 13 findings that no example can fix, exactly the
"count to chase to zero by adding decorative includes" failure Capy's guide warns
against. The five `*.intro.adoc` landing pages stay in D2's scope and keep failing
it, for the same documented reason Capy's do: they introduce no type.

**`sentence-length.mjs`'s `advisoryDirs` (C2).** Capy's `9.design/` and
`A.specification-methods/` do not exist here. Set `2.networking-tutorial/`
advisory and keep every other page hard. Measured justification: that one chapter
holds 72 of the 143 page-level C2 hits and is essay-style prose teaching protocol
theory, which is precisely the material C2 relaxes for. This drops the hard page
slice from 143 to 71. Docstrings stay hard at 61.

**`doc-lint.mjs`'s A6 check.** Assert `quick-start.adoc` sits within the first
three top-level `nav.adoc` entries, unchanged in substance from Capy.

**C.1 terminology table.** Capy's rows are coroutine vocabulary and carry over
verbatim. Corosio needs networking rows added rather than inherited — the
candidates to settle while writing the guide are one term each for: the
`tcp_acceptor`/listening-socket concept, `endpoint` vs address vs peer, the
mock-socket testing vocabulary (`mocket`, socket pair), and TLS context vs stream.
Extend the table; do not let synonyms drift.

### 4.3 `mrdocs-warnings.mjs` — ported, and its version pin removed

**This section previously said the pin needed no edit. That was wrong, and the first
CI reseed proved it.** The claim rested on measuring the MrDocs binary on a developer
machine, which reported `0.8.0+<sha>`; `mrdocsBaseVersion()` strips the `+` metadata,
so base `0.8.0` matched `PINNED_VERSION`. The mistake was generalising from that: the
`develop-release` asset is a moving target, and the same asset name reported

```
0.8.0+e31308f6c944     (local, downloaded by doc/build_antora.sh)
2026.9.5               (CI, days later, same asset)
```

Upstream moved to a date-based version. The pin then rejected the only candidate, the
check reported SKIPPED, and the reseed candidate carried `mrdocs_warnings: 0` against
a 460-fingerprint gated baseline — which would have wiped the whole MrDocs gate.
`baseline-diff.mjs` refused the candidate for exactly that reason, which is the
F4 fail-closed rule doing its job.

The fix is not a new number. **`MRDOCS_ROOT`, when set, is authoritative and gets no
version check.** `doc/build_antora.sh` installs the reference-snippets extension into
exactly one MrDocs and exports `MRDOCS_ROOT`, writing it to `$GITHUB_ENV` so it
survives into later workflow steps. That install is the one the rendered reference was
generated with, which is the invariant the check needs: measure the reference surface
with the same MrDocs that produced it. Pinning a version against a rolling asset
cannot express that and can only fail.

`PINNED_VERSION` survives as a tiebreaker for the fallback cache scan, where the
reference-collector cache can hold several binaries (the `develop` and `master` tags)
and picking the first would be nondeterministic. `MRDOCS_VERSION` still overrides it.
The resolved binary, its reported version, and which of the two paths chose it are
emitted in the payload, so a future scheme change shows up in the run log instead of
turning into another silent skip.

### 4.4 `run-a11y.mjs` — not ported

In Capy this check cannot fail CI: the step carries `continue-on-error: true`, E4
is Review tier in Part F.0, it appears in no `--gate` spec, and the gate's own
comment states that a skip of the a11y scan does not fail the gate. Per the
maintainer's rule — a check that cannot fail CI is not needed — it does not come
across, and neither does the `pa11y-ci` dependency, `.pa11yci.json`, or the
`PA11Y_CHROME_PATH` job env. Corosio's `STYLE_GUIDE.md` E4 entry is written as
review-by-eye with no scan, rather than copying Capy's "scan runs non-blocking"
wording.

### 4.5 Workflow changes

`.github/workflows/docs.yml` gains, after the existing site build and the blocking
reference-examples check:

| Step | `continue-on-error` |
|---|---|
| install `asciidoctor` (apt, before every Vale step) | false |
| install Vale 3.15.1 | true |
| `vale sync` | true |
| `vale modules` | true |
| `node lint/extract-docstrings.mjs && vale lint/.docstrings` | true |
| `node lint/doc-lint.mjs` | true |
| `node lint/check-include-tags.mjs` | **false** |
| `node lint/mrdocs-warnings.mjs` | true |
| `node lint/selftest.mjs` | true → **false** at phase 9 |
| `node lint/check-no-new-violations.mjs --show-baseline` | true |
| the gate, `check-no-new-violations.mjs --gate …` | false; **no `--strict` until phase 9** |
| reseed candidate steps, `workflow_dispatch` only | false |

The `asciidoctor` install is load-bearing and must precede every Vale step. Vale
3.x shells out to the **Ruby** CLI to parse AsciiDoc; the JS `@asciidoctor/core`
that `npm ci` pulls in provides no such binary. Without it Vale exits 2 having
printed nothing, which greps identical to a clean run — the F.4 failure shape.
Because both Vale checks are gated, that silent skip must fail the gate rather
than pass it.

Gate specs, with the fingerprint shapes that make them work:

```
--gate 'doc_lint:^(A1|A6|B2|D2|ANCHOR):'
--gate 'vale_adoc:Corosio\.PartHeadings$'
--gate 'mrdocs_warnings:.*'
--gate 'sentence_length:^C2:'
--gate 'vale_adoc:(Corosio\.SimpleTense|Corosio\.NoFluff|Corosio\.Terminology)$'
--gate 'vale_docstrings:(Corosio\.SimpleTense|Corosio\.NoFluff|Corosio\.Terminology)$'
```

The two shapes differ and the difference is load-bearing. `doc_lint` and
`sentence_length` fingerprints put the rule at the **head**, so `^` is correct
there. Vale fingerprints are `file:#N:Check.Name` with the check name at the
**tail**, so those specs tail-anchor with `$` and must carry no leading `^`. An
`^`-anchored Vale spec matches nothing and still reports `gated: true,
gatedNew: 0` — a gate that says it is gating while checking nothing. Capy measured
that failure twice.

## 5. F4 obligations

Part F4 records twelve checks that looked healthy while checking less than they
appeared to, every one of which read as a pass. Porting them re-inherits that
risk, so no ported check is believed on the strength of a green run.

**For every check, plant a violation of that exact rule and watch the check fail
before the check is considered adopted.** Minimum bite-test set, recorded with its
results in `doc/lint/README.md`:

| Check | Planted violation |
|---|---|
| A1 | a page with `:page-mode: concept` (a non-mode value, not merely a missing attribute) |
| A6 | `quick-start.adoc` moved to the 4th top-level nav entry |
| A7 / `Corosio.PartHeadings` | a `== Part 3: …` heading — Capy's version of this rule matched **zero** inputs for months because Vale's heading scope strips the `==` markers |
| B2 | raw code in a bare `----` listing, in a 5-dash listing, and under a `[source,cpp]` + `[role=output]` pair (the laundering case) |
| ANCHOR | `` `[[nodiscard]]` `` in prose |
| C2 | a 30-word sentence in a docstring and one in a hard-slice page |
| C4/C9/C10 | one Vale hit per rule, on **both** corpora |
| the gate | each `--gate` spec in turn, confirming a *new* planted finding is reported and a baselined one is not |
| fail-closed | a gated check forced to zero findings against a non-empty baseline must be **fatal**, not a pass — `vale --output=JSON lint/.nonexistent-corpus` prints `{}` and exits 0, and zero looks exactly like success |
| extractor | `extract-docstrings.mjs` made to crash must not leave a stale corpus linting clean; `baseline.mjs` checks its exit status for this reason |

`selftest.mjs` automates the subset it can and is the standing guard afterwards.
It stays non-blocking during the burn-down and becomes blocking at phase 9.

## 6. Remediation phases

Ordered by increasing prose risk. Each phase ends with a CI reseed of
`baseline.json` via `workflow_dispatch` — never locally, because a local run
grandfathers hundreds of local-vs-CI drift fingerprints — and, where the phase
takes a rule to zero, promotion of that rule in the gate spec.

### Phase 0 — infrastructure

Port everything in section 4, seed `baseline.json` in CI, wire the report-only
gate, and complete the section 5 bite-tests. **Exit**: CI green; every bite-test
confirmed failing its own check; `README.md` records the results.

### Phase 1 — mechanical, no prose risk

| Item | Count | Detail |
|---|---|---|
| A1 | 48 | add `:page-mode:` to all 48 pages — none has one today. Modes: `2.networking-tutorial/*` → `explanation` (see the correction in 1a); `3.tutorials/*` → `tutorial`; `4.guide/*` and `5.testing/*` → `how-to`; `*.intro.adoc`, `index.adoc`, `benchmark-report.adoc` → `explanation`; `glossary.adoc` → `reference`; `quick-start.adoc` → `tutorial`. Values must be one of the four Diátaxis modes — A1 checks the value, not just presence. |
| E2 | 1 | add `page-toc: ''` and `toclevels: 2` to `doc/antora.yml`'s `asciidoc.attributes`, matching Capy's |
| A6 | 1 | move `quick-start.adoc` to the 2nd top-level `nav.adoc` entry, directly after `index.adoc` |
| ANCHOR | 2 | `4.guide/4e.tcp-acceptor.adoc:75`, `4.guide/4m.error-handling.adoc:23` → passthrough `` `+[[...]]+` `` |
| B5 | 3 | `snippets/3d_tls_context.cpp:47`, `snippets/4l_tls.cpp:53`, `snippets/4g_composed_operations.cpp:74` → replace the using-directive with the `corosio`/`capy` namespace alias and qualify the names. The seven `using namespace std::chrono_literals;` instances are **not** B5 violations and stay. |

**Exit**: A1, A6, ANCHOR at zero; promote each to a strict gate spec entry.

### Phase 2 — B2/B3 retagging

All 39 findings, classified by inspection (full list in appendix A). None needs a
new compiled example; all need correct tags.

| Kind | Count | Fix |
|---|---|---|
| Bare listings holding hand-drawn figures and notation — dotted quads, IPv6 forms, URL anatomy, connection four-tuples, ASCII handshake ladders, the sliding-window diagram, bandwidth-delay arithmetic | 19 | add `[role=figure]` |
| `[source,bash]` / `[source]` blocks holding terminal transcripts and literal output — `$ ./echo_server 8080 10`, telnet sessions, throughput tables | 18 | **drop the `[source,*]` attribute line**, then add `[role=output]` |
| `[source,cmake]` build snippets in `4.guide/4l.tls.adoc` | 2 | keep `[source,cmake]`, add `role=external` |

The middle row is the one to get right. B3 is explicit that `role=output` exempts
**only a bare listing, never a `[source,*]` block** — a block that actually
compiles is tagged `[source,*]` and cleared through `pseudocode`/`external`, full
stop, however output-shaped it looks. Adding `role=output` to a surviving
`[source,bash]` line would be the laundering case B3 exists to forbid, and the
bite-test in section 5 covers it.

`doc-lint.mjs`'s SHAPE heuristic runs over every block phase 2 exempts and is
advisory. Read its output at the end of the phase: a `role=figure`/`role=output`
tag is a permanent B2 exemption, so a wrong tag is a permanent blind spot.

**Exit**: B2 at zero, SHAPE reviewed and clean; promote B2 to a strict gate entry.

### Phase 3 — B1 `cpp:` conversion

214 backtick spans naming 59 distinct `boost::corosio` public symbols become
`cpp:boost::corosio::X[]`. The remaining 521 spans stay backticks: 68 `capy::`/
`cond::` (no reference pages exist for them, per D-3), 122 `std::` names and
uppercase macros, and 331 non-symbol words.

Largest by occurrence: `io_context` (38), `tcp_server` (13), `tcp_socket` (10),
`write_some` (8), `read_some` (8), `tls_context` (6), `tcp_acceptor` (6),
`stream_file` (6).

Generate candidates by intersecting backtick spans against the MrDocs tag file
rather than against a hand-written symbol list, so the set is machine-decidable
and re-runnable. Then review by hand, because a member-function span is ambiguous:
`` `recv` `` (11 occurrences), `` `send` `` (9), `` `send_to` ``, `` `recv_from` ``,
`` `set_option` ``, `` `shutdown` ``, `` `peer` `` each need the owning class
resolved from context before the macro can be written, and several are used as
plain English verbs in the networking tutorial rather than as symbol references.
Convert the unambiguous class and free-function names mechanically; take the
member names page by page.

**Exit** (as delivered): every backticked span naming a Corosio public entity is a
`cpp:` macro (186), plus 30 bare names that were unambiguous — 28 bold type names in
feature lists and 2 in a thread-safety table. `accept.txt` absorbs the genuine prose
words. Verified against the built site: zero unresolved-reference warnings, zero
literal `cpp:` strings in the HTML.

**Docstring half (added after the phases were first declared done).** B1 was
originally scoped to `doc/modules/ROOT/pages` only, which left 214 bare Corosio
identifiers in header docstrings — the same defect class, in the published
reference. The remedy there is a code span, not a `cpp:` macro: the house convention
runs 1179 backticks to 197 `@ref`, and `@ref` cannot resolve a name with no Corosio
reference page (`operation_canceled` is a `std::errc`). 93 were backticked in the
public headers; `detail/` was left alone because `extract-docstrings.mjs` excludes it
as implementation-defined.

Most of the backlog turned out not to be a docstring defect at all. Three extractor
fixes — `@ref`/`@p`/`@c` and `@see` targets re-emitted as code spans, `@par !example`
directives dropped — removed findings that could never have been fixed in a header,
because backticking them there would break a link or a parameter binding. Corpus
420 → 62, `Vale.Spelling` 389 → 24. Details in `doc/lint/README.md`.

**Residual, deliberate.** About 26 bare type names stay unlinked. Most are the concept
used as an English noun — "binds to a local endpoint", "the resolver may return
multiple endpoints", "both sides read and write" — where a reference link would be
wrong rather than merely noisy. The rest sit in headings, and Capy keeps `cpp:` macros
out of headings: a linked heading changes the ToC text and the anchor. A further 7 are
possessives of acronyms (`IP's`, `UDP's`, `URL's`) and one is a variable name (`ec`)
that wants a code span. `Vale.Spelling` is not a gated check, so all of these are
reported without blocking. Do not silence them by adding symbol names to `accept.txt`
— that file's own rule forbids it, and it would hide genuine defects elsewhere.

### Phase 4 — C11 docstring commands

41 `@par Preconditions` become `@pre`, across 17 headers. Heaviest:
`detail/timer.hpp` (7), `io_context.hpp` (5), `tcp_acceptor.hpp` (4),
`{wolfssl,openssl,tls}_stream.hpp` (3 each), `tcp_socket.hpp` (3). The two forms
do not survive extraction in the same shape — `@par Preconditions` re-emits as a
bare "Preconditions" prose line, `@pre` re-emits with no label — so confirm the
rendered reference still reads correctly after the change, not just that the
docstring compiles.

**Exit**: `grep -r '@par Preconditions' include/` is empty.

### Phase 5 — C9 fluff and C10 terminology

9 fluff hits — `simply` ×5, `utilize` ×2 and `essentially` ×1 on pages, `note that`
×1 in a docstring — and 18 terminology hits, 10 on pages and 8 in docstrings,
**every one** of them `launch`/`spawn` where C.1 requires **start**: `3a.echo-server.adoc` (2), `3e.hash-server.adoc` (3),
`4b.concurrent-programming.adoc` (3), `2i.tcp-connections.adoc`,
`4c.io-context.adoc`, plus 8 in docstrings.

Check each `scheduler` occurrence (10 on pages) individually: C.1 approves the
word only in its P2300 sense and never as a synonym for **executor**.

**Exit**: `Corosio.NoFluff` and `Corosio.Terminology` at zero on both corpora;
promote both to strict gate entries.

### Phase 6 — C4 present simple

253 hits: 186 in docstrings, 67 on pages. Docstrings first — they are the larger
share, and reference briefs are where C4 is enforced hard. Pages after, and within
pages `2.networking-tutorial` holds 41 of the 67.

C4 has no advisory tier; every hit is in scope. This is the first phase that
changes prose meaning, so it wants review in reviewable slices rather than one
sweep.

**Exit**: `Corosio.SimpleTense` and `Google.Will` at zero on both corpora; promote
to strict gate entries.

### Phase 7 — C2 sentence length

Hard slice after the phase-0 `advisoryDirs` change: 71 page hits + 61 docstring
hits. `2.networking-tutorial`'s 72 are advisory and are not a backlog.

C2's authority is `lint/sentence-length.mjs`, not Vale.
`Corosio.SentenceLength` is `level: suggestion` and enforces nothing; do not read
`vale`'s exit code for C2.

**Exit**: `sentence_length`'s `hard` count at zero; promote `^C2:` to a strict gate
entry. `advisory-C2` stays ungated by design.

### Phase 8 — B4 briefs

Roughly 30 identity-shaped briefs, mostly class briefs opening "A…" / "An…" /
"The…". The guide's B4 entry carries an explicit reversal: two prior Capy audits
read identity-shaped class briefs as house convention and dropped ~230 findings
each on that reading, and the maintainer ruled B4 binds them anyway. Corosio's
guide must carry that ruling, and this phase applies it: a brief says what the
entity *does*.

B4 is Review tier — no script decides it. It is a PR-checklist item and a
reviewed pass, not a gate.

**Exit**: every public class and function brief describes behavior. Reviewed, not
measured.

### Phase 9 — close the gate

Flip the gate step to `--strict`, promote `selftest.mjs` to
`continue-on-error: false`, reseed once more, and confirm the residual baseline
holds only the documented carve-outs: `advisory-C2`, D2's `*.intro.adoc` findings,
and `2.networking-tutorial`'s primer exemption.

**Exit**: a planted violation of each strict-gated rule fails the job. Per F4, the
green run before that test is not the evidence — the failure is.

## 7. Out of scope, and what that costs

**Capy's enforcement gaps.** Only two steps in Capy's docs.yml can fail the job:
the reference-examples grep and `check-include-tags.mjs`. Its Vale checks,
`doc-lint.mjs`, `mrdocs-warnings.mjs`, `selftest.mjs`, and the gate itself are all
advisory — the gate runs without `--strict` by deliberate maintainer decision. The
follow-ups this plan does not own: remove `run-a11y.mjs` and `pa11y-ci` from Capy
per section 4.4's reasoning, and restore `--strict` plus a blocking `selftest.mjs`
there. Until then Capy and Corosio diverge, and Corosio is the stricter of the two
from phase 9 onward.

**The report-only window.** D-5 keeps the gate advisory through phases 1–8. A new
violation introduced in that window is reported and annotated but lands, and stays
until a reseed grandfathers it. The mitigation is the per-phase reseed cadence in
section 6, which keeps the window short per rule rather than open for the whole
effort.

**Deferred rules.** E3 (reference grouping, operators documented with their types,
async distinguishable from sync) and E4 are Review tier and are not scheduled
here. D1, D3, D4, D5, A3, A4, A5, C7, C8 are likewise Review tier; they enter via
the F3 PR checklist, which Corosio's `STYLE_GUIDE.md` will carry, rather than as
phases.

## Appendix A — B2 block classification

`[role=figure]` (19):

```
2.networking-tutorial/2b.internet-addresses.adoc     18, 36, 68, 74
2.networking-tutorial/2d.urls.adoc                   18, 42, 93
2.networking-tutorial/2e.client-server-model.adoc    40, 46, 52
2.networking-tutorial/2i.tcp-connections.adoc        22, 55
2.networking-tutorial/2j.tcp-data-flow.adoc          37
2.networking-tutorial/2k.tcp-reliability.adoc        48
2.networking-tutorial/2l.tcp-performance.adoc        36
4.guide/4a.tcp-networking.adoc                       151, 305, 360
4.guide/4b.concurrent-programming.adoc               303
```

Drop `[source,*]`, add `[role=output]` (18):

```
3.tutorials/3a.echo-server.adoc                      147, 155
3.tutorials/3b.http-client.adoc                      131, 140
3.tutorials/3c.dns-lookup.adoc                       114, 124
3.tutorials/3e.hash-server.adoc                      160, 168
3.tutorials/3f.reconnect.adoc                        167, 175, 183, 197
quick-start.adoc                                     80
benchmark-report.adoc                                281, 843, 954, 1039, 1117
```

Keep `[source,cmake]`, add `role=external` (2):

```
4.guide/4l.tls.adoc                                  525, 533
```

Line numbers are from the pre-remediation tree and shift as phases land; re-run
`node lint/doc-lint.mjs` for current positions rather than trusting these.
