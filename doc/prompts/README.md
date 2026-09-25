<!--
    Copyright (c) 2026 Michael Vandeberg

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

    Official repository: https://github.com/cppalliance/corosio
-->
# Documentation Prompt Collection

A collection of structured prompts (in the `tools-public` house style) that generate,
repair, and audit Corosio documentation. They were ported from Capy's `doc/prompts`, which
remains the origin for the shared design rationale; what is Corosio-specific is recorded
here. The prompts do the work a linter cannot:
they own the **judgment rules** — Diátaxis mode purity, duplication, objectiveness,
pedagogy, and code↔doc drift — while deterministic tooling owns the mechanical rules.

**Two surfaces.** Documentation lives as exposition `.adoc` pages *and* as reference
docstrings in the headers (MrDocs generates the reference from them). Every tool applies the
five axes to both: `doc-audit` gains a reference mode (fixed Diátaxis mode = `reference`,
axes remapped to the docstring contract); `doc-write`/`doc-fix` accept a header `target_file`
and write/repair the docstring in the `.hpp`; `doc-sync` is the natural home for reference
drift — a changed symbol and its docstring share one diff, so the co-located docstring is
always its first drift hit. **A reference edit lands in the `.hpp`, never a generated page.**
Docstring conventions follow the `boost-docs` skill.

## The two ends of the pipeline

```
        GENERATE  ─────────────────────────────►  DETECT
        doc-write            doc-sync            doc-audit
      (new page to spec)  (repair code-change   (score existing
                            drift)                pages on 5 axes)
                                │                      │
                                └──────► doc-fix ◄──────┘
                                      (apply grounded edits
                                       from findings)
```

| Tool | End | Trigger | Output |
|---|---|---|---|
| **doc-write** | generate | a symbol/feature/topic to document | a new `.adoc` page + compiled snippet(s) + nav entry |
| **doc-sync** | detect + fix | a code change (diff / commit range) | edits that repair docs the change made stale |
| **doc-audit** | detect | existing or new pages | ranked findings on the five axes |
| **doc-fix** | fix | findings (from `doc-audit` or `doc-sync`) | a minimal, grounded patch set |

`doc-sync` and `doc-audit` both hand their findings to `doc-fix` for repair, so the edit
contract lives in one place.

## Shared rubric

Every tool references one source of truth: the project **documentation style guide**
(`doc/STYLE_GUIDE.md`). The style guide defines:

- the **five axes** — **St**ructure, **Ac**curacy, **Wo**rding, **Co**mpleteness & Pedagogy,
  **Pr**esentation & Tooling;
- the **Diátaxis** mode taxonomy (tutorial / how-to / reference / explanation);
- the **single-source-of-truth** rules (link the reference via `cpp:`; include compiled
  snippets, never paste code);
- the **terminology table** (one term per concept).

The tools cite the guide by section (e.g. "style guide C.1" for terminology) rather than
restating it, so the rubric never drifts from the guide.

## Division of labor — what these tools do NOT do

Deterministic tooling owns the mechanical rules and is a separate CI gate:

- **Vale** — banned words and terminology substitutions (`doc/.vale/styles/Corosio/*`). It does
  **not** enforce C2.
- **snippet-compile job** — every documentation code block compiles against the real API.
  `doc/lint/check-include-tags.mjs` additionally proves every `[tag=…]` resolves to a live
  tag in a compiled source.
- **`doc/lint/sentence-length.mjs`** — the authority for C2, no sentence over 25 words,
  over both corpora.
- **`doc/lint/mrdocs-warnings.mjs`** — the reference surface: undocumented symbols,
  missing `@param`/`@return`, broken `@ref`.
- **structural lint script** (`doc/lint/doc-lint.mjs`) — mode-attribute presence, nav
  position, "no raw `[source]`
  blocks", "every concept page has an `include::example$`".

`doc/lint/README.md` describes how those gates are wired;
`doc/lint/check-no-new-violations.mjs` is the gate that compares a fresh run against
`doc/lint/baseline.json`. These prompts assume those gates exist and target only what
they cannot check.

## Shared invariants (every tool obeys)

1. **Raw code and raw page prose never enter the main context.** Sub-agents read from disk;
   the main context orchestrates over structured JSON records only.
2. **Every claim is grounded.** A statement about the API is backed by a verbatim quote of
   the real declaration or reference; no claim is invented.
3. **Single source.** Code blocks are `include::example$…[tag=…]` of compiled sources, never
   hand-typed. Signatures are `cpp:` links, never restated in prose.
4. **Noise floor.** A finding or edit without a verbatim span is discarded. Subjective
   preference is not a finding. Doing nothing is a valid outcome.

## Sub-agent dispatch contract

Invariant 1 above is a promise; this section is the mechanism that keeps it, stated fully
inside this collection (no external tool or file is required to understand or run it).

1. **Step 0 is deterministic and runs in the orchestrator** (what each tool calls "the main
   context") — no LLM call. It only computes paths, an inventory, a change set, or a brief
   from arguments and file **listings** (names, diff stats) — never file **contents**.
2. **One sub-agent per unit** — one page, one symbol, one doc hit, one finding, one edit. The
   orchestrator's dispatch to that sub-agent carries only identifiers already produced by a
   prior step: a `path`/`symbol`, and the prior step's typed JSON record. It never carries
   file contents, because the orchestrator never held any to begin with.
3. **The sub-agent is the only actor that reads raw content.** It opens the file(s) itself,
   from disk, using its own tools. Whatever it reads exists only inside that sub-agent's own
   context — the orchestrator has no channel into it.
4. **The sub-agent's only return value is its step's typed JSON record**, validated against
   that step's schema before the orchestrator accepts it. A record's only raw-text fields are
   the short, capped verbatim spans the schema itself demands (`span`, `source_span`,
   `evidence` — ≤ 200–300 chars, with the single stated C1/C2 exception) — never the
   surrounding page, docstring, or diff hunk.
5. **The adversarial challenge/verify stage is a separate sub-agent dispatch**, not a
   continuation of the authoring sub-agent's context. It receives the same kind of
   identifier-plus-record input, re-reads the file from disk **independently**, and returns
   its own typed verdict record. It never receives the first sub-agent's raw reading — only
   its claim.
6. **Consequence:** the orchestrator's own context, across an entire run, contains nothing
   but paths, counts, and validated JSON records carrying capped verbatim spans. There is no
   step at which an orchestrator instruction says "read this file and show me its contents" —
   every read happens inside a sub-agent whose one output channel is the schema. Raw code and
   raw prose structurally cannot reach the orchestrator under this contract.
