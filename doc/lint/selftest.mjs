#!/usr/bin/env node
//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//
// selftest.mjs — asserts that sentence-length.mjs and doc-lint.mjs's B2 check
// still detect what they claim to, against the checked-in corpus in
// lint/fixtures/ (sentence-length.mjs) or a throwaway fixture tree built at
// run time (doc-lint.mjs's B2 section). Node built-ins only. Exit 0 = all
// assertions hold; exit 1 = at least one broke, with the failure named. Run
// it after any edit to sentence-length.mjs or doc-lint.mjs.
//
// Why this exists. The C2 checker is on its way to becoming a merge-blocking
// gate, and the properties below are exactly the ones whose failure is SILENT:
// nothing in the real corpus exercises them, so a plausible refactor can retire
// a protection and every downstream number still looks reasonable. Two such
// refactors were demonstrated on the unbalanced-backtick guard alone — making
// the backtick pattern lenient returns the diagnostic count to 0 and makes a
// 30-word sentence vanish with no finding at all, and renaming the rule's `id`
// without updating maskBlock()'s skip string keeps the diagnostic but silently
// drops the count correction. Both pass every corpus-level check. Neither passes
// this file.
//
// Fixtures live in lint/fixtures/ and are NOT part of either linted corpus:
// Vale runs on `modules` and `lint/.docstrings` only, and Antora reads
// modules/ via antora.yml, so nothing else sees them.
//
// Usage: node doc/lint/selftest.mjs
//
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DOC_DIR = path.resolve(SCRIPT_DIR, '..');
const FIXTURES = path.join(SCRIPT_DIR, 'fixtures', 'modules');

const r = spawnSync('node', [path.join(SCRIPT_DIR, 'sentence-length.mjs'), FIXTURES],
  { encoding: 'utf8', cwd: DOC_DIR, maxBuffer: 16 * 1024 * 1024 });
if (r.status !== 0) {
  console.error(`selftest: sentence-length.mjs exited ${r.status}\n${r.stderr || r.stdout}`);
  process.exit(1);
}
const out = JSON.parse(r.stdout);
const hard = out.findings.C2;
const advisory = out.findings['advisory-C2'];
const backtick = out.findings.BACKTICK;

// A finding is identified by the first word of its sentence, which is unique per
// fixture and survives a change to line numbering.
const lead = (f) => f.sentence.replace(/^[^A-Za-z`]*/, '').split(/[\s`]+/)[0];
const find = (arr, word) => arr.filter((f) => lead(f) === word);

const failures = [];
// Counted, not hand-written: a literal here goes stale silently, and a total
// nobody can trust is worse than no total. `check` tallies itself and the
// summary reports the tally. one()/none() delegate to check(), so they are
// counted there.
let assertionCount = 0;
function check(name, cond, detail) {
  assertionCount++;
  if (cond) return;
  failures.push(`${name}${detail ? ` — ${detail}` : ''}`);
}
function one(name, arr, word, words) {
  const hits = find(arr, word);
  if (hits.length !== 1) {
    assertionCount++;
    failures.push(`${name} — expected exactly 1 finding leading with '${word}', got ${hits.length}`);
    return;
  }
  check(name, hits[0].words === words, `'${word}' measured ${hits[0].words} words, expected ${words}`);
}
function none(name, arr, word) {
  const hits = find(arr, word);
  check(name, hits.length === 0,
    `expected no finding leading with '${word}', got ${hits.length} (${hits.map((h) => `${h.words}w`).join(', ')})`);
}

// 1. The unbalanced-backtick guard. Both halves matter: the diagnostic must be
//    emitted AND the sentence must still be measured at its full length. A
//    lenient backtick pattern loses both; a stale skip id loses only the second.
one('backtick guard: sentence measured at full length', hard, 'One', 30);
check('backtick guard: diagnostic emitted', backtick.length === 1,
  `expected 1 BACKTICK finding, got ${backtick.length}`);
check('backtick guard: summary counts it', out.summary.unbalancedBackticks === 1,
  `summary.unbalancedBackticks = ${out.summary.unbalancedBackticks}`);

// 2. The two UNDER-reporting cases. These are the only known ways a real
//    violation can slip through, so they are the assertions that protect the
//    gate's floor rather than its ceiling.
one('mid-sentence ellipsis does not split the sentence', hard, 'Alpha', 34);
one('parenthesised abbreviation does not split the sentence', hard, 'Aone', 31);

// 3. Bold run-in lead is its own sentence, so neither half is over the limit.
none('bold run-in lead does not merge into the next sentence', hard, 'The');

// 4. Reader word counting. 30 tokens under the retired Vale token, 25 as a
//    reader counts, so any regression in the connector set makes this fire.
none('reader word counting keeps a 25-word sentence under the limit', hard, 'most-derived');

// 5. Code blocks are not prose.
check('code blocks are not linted as prose',
  !JSON.stringify(out.findings).includes('utilize_and_leverage'),
  'a [source,cpp] block reached the linter');

// 6. The hard/advisory partition, in both directions, including the look-alike
//    directory that must NOT be treated as an essay.
one('2.networking-tutorial/ findings are advisory', advisory, 'Advisory', 28);
none('2.networking-tutorial/ findings are not in the hard slice', hard, 'Advisory');
one('a 2.networking-tutorialish/ look-alike stays in the hard slice', hard, 'Lookalike', 28);

// 7. The gate-reachability property itself, tested on the rule keys rather than
//    asserted in a comment: a `^C2:` spec must reach the hard key and nothing
//    else, whatever the keys are renamed to.
const GATE = /^C2:/;
const keys = Object.keys(out.findings);
check('rule keys are C2 / advisory-C2 / BACKTICK', keys.join(',') === 'C2,advisory-C2,BACKTICK',
  `got '${keys.join(',')}'`);
check('only the hard key is reachable from a ^C2: gate spec',
  keys.filter((k) => GATE.test(`${k}:some/file.adoc:#1:message`)).join(',') === 'C2',
  `reachable keys: '${keys.filter((k) => GATE.test(`${k}:f:#1:m`)).join(',')}'`);

// 8. extract-docstrings.mjs covers BOTH Doxygen comment forms. `///` runs were
//    invisible to every gate until 2026-08 — 86 published doc lines across 25
//    headers, and the gap surfaced only because a bite test happened to plant its
//    first probe in a `///` comment. A tightened regex or a reverted branch would
//    retire the coverage silently: the corpus just gets smaller, every count drops,
//    and nothing reads as broken. The expectations below are DERIVED from the real
//    header tree rather than written down, so they cannot go stale.
const FIXTURE_INCLUDE = path.join(SCRIPT_DIR, 'fixtures/include');
const TMP_OUT = fs.mkdtempSync(path.join(os.tmpdir(), 'corosio-selftest-docstrings-'));
try {
  // Run against a FIXTURE header tree, not the live one. The old form derived its
  // sample from the real corpus — "headers with `///` and no `/** */`" — which
  // silently depended on which headers happened to exist. Every such header lived
  // under detail/, so excluding detail/ from extraction broke the assertion for a
  // legitimate reason. A fixture cannot go stale that way.
  const x = spawnSync('node',
    [path.join(SCRIPT_DIR, 'extract-docstrings.mjs'), TMP_OUT, FIXTURE_INCLUDE],
    { encoding: 'utf8', cwd: DOC_DIR, maxBuffer: 16 * 1024 * 1024 });
  check('extract-docstrings.mjs exits 0', x.status === 0,
    `exited ${x.status}: ${(x.stderr || '').trim().slice(-200)}`);

  const out = path.join(TMP_OUT, 'slash_only.hpp.adoc');
  const got = fs.existsSync(out) ? fs.readFileSync(out, 'utf8') : '';
  check('`///` doc comments are extracted', got.includes('Reports whether the fixture extracted'),
    'the `///`-only fixture header produced no output — the `///` branch is not firing');
  check('`///` continuation lines fold into one comment',
    got.includes('folded into the same doc comment'), 'second `///` line was dropped');
  // detail/ is excluded by directory; this covers the OTHER half — `namespace detail`
  // inside an otherwise-public header, which 5 real headers have.
  check('doc comments inside `namespace detail` are NOT extracted',
    !got.includes('must NOT be extracted'),
    'a `namespace detail` doc comment reached the linted corpus');
  // The other half of the exclusion: a whole file under detail/. Covered
  // separately because the `namespace detail` fixture cannot reach it — a
  // mutation removing the directory skip passed until this fixture existed.
  check('headers under detail/ are NOT extracted at all',
    !fs.existsSync(path.join(TMP_OUT, 'detail/excluded.hpp.adoc')),
    'a header under detail/ produced output in the linted corpus');
} finally {
  fs.rmSync(TMP_OUT, { recursive: true, force: true });
}

// 9. doc-lint.mjs's B2 check ("no code block holds raw code") must reach
//    every [source,<lang>] block, not just [source,cpp]/[source,c++] — that
//    was the whole gap a prior widening closed — and must also reach bare
//    `----`/`....` listings, while leaving role=pseudocode/external/
//    output/figure and include::example$ blocks alone. There was previously
//    no self-test coverage for doc-lint.mjs at all, so a regex narrowed back
//    to one language, or a bare-listing branch that stopped firing, would
//    pass every corpus-level check silently. Exercised against a throwaway
//    fixture tree (not lint/fixtures/, which only sentence-length.mjs reads)
//    so a real corpus edit can't perturb these counts.
//
//    flagged.adoc's `[source,cmake]` case is a weaker property than its name
//    suggests: narrowing isSource back to cpp/c++-only does NOT clear that
//    finding, because a de-recognized [source,cmake] block still falls into
//    the bare-listing branch and gets flagged there instead (same result,
//    different code path). The real proof that isSource covers every
//    language lives on the CLEARING side, in clear.adoc: a
//    [source,cmake,role=pseudocode] block is invisible to B2 only if
//    isSource recognizes cmake — if it doesn't, that block falls into the
//    bare-listing branch too, where role=pseudocode is NOT a recognized
//    non-code role, and it lights up clear.adoc instead. That is where an
//    isSource regression actually surfaces; flagged.adoc's cmake case is
//    kept only because catching the "still gets flagged, for the wrong
//    reason" case is itself worth asserting.
{
  const DOCLINT_TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'corosio-selftest-doclint-'));
  try {
    const write = (rel, body) => {
      const fp = path.join(DOCLINT_TMP, rel);
      fs.mkdirSync(path.dirname(fp), { recursive: true });
      fs.writeFileSync(fp, body);
    };
    // One [source,cmake] block with no clearing role (see the comment
    // above — flagged via isSource OR the bare-listing fallback, either
    // way); one bare `----` block with no role=output/role=figure marker
    // and real code in it (bare listings were invisible to any
    // [source,...] regex before B2 was widened); and one [source,cpp,
    // role=output] block — role=output/role=figure must clear ONLY a bare
    // listing, never a [source,*] block. A mutation that ORs hasClearingRole
    // and hasNonCodeRole together (ignoring isSource) makes role=output a
    // blanket exemption for real C++ and this block stops being flagged.
    write('flagged.adoc', [
      ':page-mode: how-to',
      '',
      '= Flagged',
      '',
      '[source,cmake]',
      '----',
      'add_executable(x x.cpp)',
      '----',
      '',
      '----',
      'int x = 1;',
      '----',
      '',
      '[source,cpp,role=output]',
      '----',
      'int y = 2;',
      '----',
      '',
    ].join('\n'));
    // Every exemption B2 recognizes, one of each, all in a single page that
    // must produce zero findings. role=pseudocode and role=external are
    // BOTH tested here deliberately: they are two different alternatives in
    // the same regex, and dropping either one independently keeps this
    // fixture passing for the OTHER unless both are exercised (dropping
    // `external` alone was a measured miss — the corpus is cleared mostly
    // by `external`, not `pseudocode`, e.g. 9k/9l/9n/9o/5d).
    write('clear.adoc', [
      ':page-mode: how-to',
      '',
      '= Clear',
      '',
      '[source,cmake,role=pseudocode]',
      '----',
      'add_executable(x x.cpp)',
      '----',
      '',
      '[source,cpp,role=external]',
      '----',
      'task<int> async_work();',
      '----',
      '',
      '[role=output]',
      '----',
      'build succeeded',
      '----',
      '',
      '[role=figure]',
      '----',
      '[A] --> [B]',
      '----',
      '',
      '[source,cpp]',
      '----',
      'include::example$foo.cpp[tag=bar]',
      '----',
      '',
    ].join('\n'));
    // SHAPE: a role=output block whose content looks like code is a
    // permanent B2 blind spot by design (that is what role=output is FOR),
    // so SHAPE must still flag it — advisory, not gated. A genuine output
    // block (no code-shaped line) must not trip SHAPE at all.
    write('shape.adoc', [
      ':page-mode: how-to',
      '',
      '= Shape',
      '',
      '[role=output]',
      '----',
      'int z = 3;',
      '----',
      '',
      '[role=output]',
      '----',
      'Hello from Corosio!',
      '----',
      '',
    ].join('\n'));
    // I1/I2: a 5-dash listing must not hide code from B2 (closer length
    // must match opener length, not just be >=4), and a `....` literal
    // block is a second bare-listing syntax B2 must also reach. The
    // 5-dash case above pairs a matching 5-dash closer with its 5-dash
    // opener, which does NOT exercise the closer-length check at all — a
    // mutation loosening it from `===` to `>=` still passes that case.
    // The role=figure block below is the real regression this fixture was
    // missing (measured against the real corpus, 9b.Separation.adoc's CCD
    // diagram): its body contains a dash-only line LONGER than its own
    // 4-dash opener. Under `===` this is correctly just content, and the
    // real closer below it ends the block with zero findings. Under `>=`
    // the long dash-only line is wrongly accepted as an early closer, and
    // the real closing `----` is then misread as a brand-new, attribute-less
    // opener with nothing after it — a false B2 finding on a block that
    // never contained code.
    write('delimiters.adoc', [
      ':page-mode: how-to',
      '',
      '= Delimiters',
      '',
      '-----',
      'int five_dash = 1;',
      '-----',
      '',
      '....',
      'int four_dot = 1;',
      '....',
      '',
      '[role=figure]',
      '----',
      '-------------------',
      'CCD = 5',
      '----',
      '',
    ].join('\n'));

    // G1 (final-review fix): AsciiDoc permits a block's attribute list to be
    // split across multiple adjacent `[...]` lines, and Asciidoctor merges
    // them into one. scanBlocks() used to read only the single nearest
    // `[...]` line above the delimiter, so a `[source,cpp]` marker one line
    // further up was invisible: `isSource` came back false, the block took
    // the bare-listing branch, and role=output cleared it as non-code — B2:0.
    // SHAPE, which still looks at bare-listing content, ALSO missed it: its
    // `;\s*$` pattern is defeated by the trailing `// running sum` comment,
    // which is this corpus's own annotation idiom for [role=output] blocks.
    // The result was a highlighted C++ source block invisible to both B2 and
    // SHAPE. Confirmed to reproduce against the pre-fix scanBlocks() (attr
    // read from the single nearest line only) before landing the fix above.
    write('split-attr.adoc', [
      ':page-mode: how-to',
      '',
      '= Split Attr',
      '',
      '[source,cpp]',
      '[role=output]',
      '----',
      'int total = 0;  // running sum',
      '----',
      '',
    ].join('\n'));

    // G1 review-round-2 fix: widening the attribute walk to ANY consecutive
    // `[...]`-shaped line (the first cut of the fix above) created a false
    // positive. A block anchor (`[[id]]`), an admonition style (`[NOTE]`),
    // or a quote attribution (`[quote,...]`) can legitimately sit directly
    // above a block with no blank line between; merging one of those ahead
    // of a real `[source,cpp,role=pseudocode]` line made the joined string
    // no longer start with `[source`, so `isSource` went false and a
    // legitimately-exempt pseudocode block was wrongly flagged as B2. The
    // walk must stop at the first line that is not itself a `[source,...]`
    // or `[role=...]` continuation. Confirmed to reproduce against the
    // review-round-1 fix (any `[...]`-shaped line merged) before landing
    // the ATTR_CONTINUATION restriction above.
    write('anchor-above-pseudocode.adoc', [
      ':page-mode: how-to',
      '',
      '= Anchor Above Pseudocode',
      '',
      '[[my-anchor]]',
      '[source,cmake,role=pseudocode]',
      '----',
      'add_executable(x x.cpp)',
      '----',
      '',
      '[NOTE]',
      '[source,cpp,role=pseudocode]',
      '----',
      'int y = 2;',
      '----',
      '',
      '[quote,Someone]',
      '[source,cpp,role=external]',
      '----',
      'task<int> async_work();',
      '----',
      '',
    ].join('\n'));

    // ANCHOR fixture. Line numbers are asserted below, so keep them stable:
    // 3 = the prose violation, 5-8 = the in-block negative, 10 = the
    // passthrough negative.
    write('attr-anchor.adoc', [
      '= Attr',                                  // 1
      ':page-mode: explanation',                 // 2
      'The wrapper is `[[nodiscard]]` here.',    // 3  <- must fire
      '',                                        // 4
      '[source,cpp]',                            // 5
      '----',                                    // 6
      'struct `[[nodiscard]]` s;',               // 7  <- must NOT fire
      '----',                                    // 8
      '',                                        // 9
      'Fixed as `+[[nodiscard]]+` instead.',     // 10 <- must NOT fire
      '',
    ].join('\n'));

    const d = spawnSync('node', [path.join(SCRIPT_DIR, 'doc-lint.mjs'), DOCLINT_TMP],
      { encoding: 'utf8', cwd: DOC_DIR, maxBuffer: 16 * 1024 * 1024 });
    check('doc-lint.mjs exits 0 against the B2 fixture tree', d.status === 0,
      `exited ${d.status}: ${(d.stderr || '').trim().slice(-200)}`);
    let dOut = null;
    try { dOut = JSON.parse(d.stdout); } catch { /* reported below */ }
    check('doc-lint.mjs prints parseable JSON', dOut !== null, `stdout: ${d.stdout.slice(0, 200)}`);
    if (dOut) {
      const flaggedHits = dOut.findings.B2.filter((f) => f.file === 'flagged.adoc');
      check('B2 catches an unmarked [source,cmake] block (directly, or via the bare-listing fallback)',
        flaggedHits.length === 3, `flagged.adoc B2 findings: ${JSON.stringify(flaggedHits)}`);
      check('B2 catches a bare `----` block holding real code with no role marker',
        flaggedHits.some((f) => f.line === 10),
        `expected a finding at flagged.adoc:10 (the bare block); got: ${JSON.stringify(flaggedHits)}`);
      check('role=output does NOT clear a [source,cpp] block holding real code',
        flaggedHits.some((f) => f.line === 14),
        `expected a finding at flagged.adoc:14 ([source,cpp,role=output]); got: ${JSON.stringify(flaggedHits)}`);
      const clearHits = dOut.findings.B2.filter((f) => f.file === 'clear.adoc');
      check('B2 leaves role=pseudocode/external/output/figure and include::example$ alone',
        clearHits.length === 0, `clear.adoc should have 0 B2 findings, got: ${JSON.stringify(clearHits)}`);

      const shapeB2 = dOut.findings.B2.filter((f) => f.file === 'shape.adoc');
      check('SHAPE-worthy blocks stay OUT of B2 (role=output is a real exemption, just not a silent one)',
        shapeB2.length === 0, `shape.adoc should have 0 B2 findings, got: ${JSON.stringify(shapeB2)}`);
      const shapeHits = dOut.findings.SHAPE.filter((f) => f.file === 'shape.adoc');
      check('SHAPE flags a role=output block whose content looks like code',
        shapeHits.some((f) => f.line === 6),
        `expected a SHAPE finding at shape.adoc:6; got: ${JSON.stringify(shapeHits)}`);
      check('SHAPE leaves a genuine role=output block alone',
        !shapeHits.some((f) => f.line === 11),
        `shape.adoc:11 is real output text, should not be SHAPE-flagged; got: ${JSON.stringify(shapeHits)}`);

      const delimHits = dOut.findings.B2.filter((f) => f.file === 'delimiters.adoc');
      check('B2 reaches a 5-dash (`-----`) listing, not just exactly `----`',
        delimHits.some((f) => f.line === 5),
        `expected a finding at delimiters.adoc:5 (the ----- block); got: ${JSON.stringify(delimHits)}`);
      check('B2 reaches a `....` literal block, not just `----`',
        delimHits.some((f) => f.line === 9),
        `expected a finding at delimiters.adoc:9 (the .... block); got: ${JSON.stringify(delimHits)}`);
      // The closer-length check is `===`, not `>=`: a role=figure block
      // whose body contains a dash-only line LONGER than its own 4-dash
      // opener must not be misread as closing early there (measured
      // against 9b.Separation.adoc's real CCD diagram, which does exactly
      // this). Exactly 2 findings total (the two above) — a third means
      // the mismatched-length body line either got flagged directly or
      // caused the real closer below it to be misread as a new opener.
      check('B2 does not misfire on a body dash-run whose length differs from its own delimiter\'s',
        delimHits.length === 2, `expected exactly 2 delimiters.adoc findings (5-dash, dot), got: ${JSON.stringify(delimHits)}`);

      // G1: a [source,cpp] block whose attribute list is split across two
      // adjacent `[...]` lines must still be recognized as source, not fall
      // through to the bare-listing/SHAPE branch.
      const splitAttrB2 = dOut.findings.B2.filter((f) => f.file === 'split-attr.adoc');
      check('B2 catches a [source,cpp] block whose attribute list is split across two lines',
        splitAttrB2.length === 1 && splitAttrB2[0].line === 5,
        `expected exactly 1 split-attr.adoc B2 finding at line 5 (the [source,cpp] line), got: ${JSON.stringify(splitAttrB2)}`);
      const splitAttrShape = dOut.findings.SHAPE.filter((f) => f.file === 'split-attr.adoc');
      check('split-attribute [source,cpp] block is caught by B2, not diverted into SHAPE',
        splitAttrShape.length === 0,
        `split-attr.adoc should have 0 SHAPE findings (B2 should catch it directly), got: ${JSON.stringify(splitAttrShape)}`);

      // G1 review-round-2: a block anchor, an admonition style, or a quote
      // attribution sitting directly above a [source,...,role=pseudocode/
      // external] block (no blank line between) must NOT be merged into
      // that block's attribute string -- only [source,...]/[role=...]
      // continuation lines may merge. All three blocks here are otherwise
      // properly exempt and must produce zero B2 findings.
      const anchorHits = dOut.findings.B2.filter((f) => f.file === 'anchor-above-pseudocode.adoc');
      check('a [[anchor]]/[NOTE]/[quote,...] line above an exempt [source,...] block does not defeat its exemption',
        anchorHits.length === 0,
        `anchor-above-pseudocode.adoc should have 0 B2 findings, got: ${JSON.stringify(anchorHits)}`);

      // ANCHOR: a C++ attribute in a prose code span. Asciidoctor's
      // inline-anchor substitution runs inside a backtick span, so
      // `` `[[nodiscard]]` `` renders as an EMPTY <code> -- the attribute
      // name vanishes from the page while the source still reads correctly.
      // That is why this needs a machine check: the defect is invisible to
      // a human reading the .adoc.
      // Both directions matter. A rule that only fired would be satisfied
      // by a whole-file regex, which would then flag every legitimate
      // `[[nodiscard]]` inside a code block -- so the in-block negative and
      // the passthrough negative are asserted too, or the block-skip could
      // be dropped in a refactor with the positive still passing.
      const anchorRule = dOut.findings.ANCHOR.filter((f) => f.file === 'attr-anchor.adoc');
      check('ANCHOR catches an attribute written as `[[...]]` in prose (renders as an empty <code>)',
        anchorRule.some((f) => f.line === 3),
        `expected an ANCHOR finding at attr-anchor.adoc:3; got: ${JSON.stringify(anchorRule)}`);
      check('ANCHOR ignores `[[...]]` inside a delimited block (literal there, renders fine)',
        !anchorRule.some((f) => f.line >= 5 && f.line <= 8),
        `attr-anchor.adoc:5-8 is inside a block and must not fire; got: ${JSON.stringify(anchorRule)}`);
      check('ANCHOR accepts the passthrough form `+[[...]]+` as the fix',
        !anchorRule.some((f) => f.line === 10),
        `attr-anchor.adoc:10 is the passthrough fix and must not fire; got: ${JSON.stringify(anchorRule)}`);
      check('ANCHOR finds exactly the one planted prose violation',
        anchorRule.length === 1, `expected exactly 1 ANCHOR finding, got: ${JSON.stringify(anchorRule)}`);
    }
  } finally {
    fs.rmSync(DOCLINT_TMP, { recursive: true, force: true });
  }
}

if (failures.length) {
  console.error(`selftest: ${failures.length} assertion(s) FAILED`);
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log(JSON.stringify({
  ok: true,
  assertions: assertionCount,
  fixtureSummary: out.summary,
}, null, 2));
