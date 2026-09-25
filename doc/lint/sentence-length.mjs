#!/usr/bin/env node
//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//
// sentence-length.mjs — the authority for Style Guide C2 ("no sentence over 25
// words"), replacing Vale's `Corosio.SentenceLength` on both documentation
// surfaces. Node built-ins only, no dependencies. JSON on stdout, in the same
// shape doc-lint.mjs uses, so baseline.mjs can fingerprint it.
//
// Why this exists rather than a Vale rule
// ---------------------------------------
// `Corosio.SentenceLength` is `extends: occurrence`, `scope: sentence`, and it runs
// AFTER doc/.vale.ini's `TokenIgnores` blanks inline code spans. Two defects
// follow from that, both measured on this branch and neither fixable inside a
// Vale rule:
//
//   1. UNDER-COUNTING. A blanked span contributes ZERO words where a reader
//      counts at least one, so the rule's 25-word budget is not the reader's.
//      Two independent Vale-side measurements agree on the size of it: task
//      P4-prereq got 140 -> 170 (+30) by rewriting `TokenIgnores` on the
//      pre-BlockIgnores-fix config, and a re-measurement on today's corpus
//      (every backtick span and `cpp:` macro outside code blocks replaced by one
//      word) got 135 -> 164 (+29).
//   2. MIS-ATTRIBUTION, which is worse. The blanking corrupts Vale's position
//      mapping for `scope: sentence` rules, so an alert can be reported against
//      the wrong block — which makes a genuinely over-limit block look
//      unreported, and it produces no alert of its own to chase. Iterating
//      `extract + vale` to a fixpoint does NOT find it. Cleanest real case,
//      hand-verified: `5.buffers/5b.types.adoc` holds two over-limit sentences in
//      list items and Vale reports NONE.
//
// So this checker does its own segmentation and its own counting, and never
// asks Vale where anything is. `TokenIgnores` in .vale.ini is deliberately left
// alone: 557 `cpp:` macros depend on it for the rules that SHOULD ignore symbol
// text. The fix is to stop depending on Vale's position mapping for C2, not to
// remove the ignores.
//
// How a code span is counted
// --------------------------
// As the one word a reader sees. A construct is masked with U+0001 runs of the
// SAME LENGTH as the original, leaving a single `x` behind, so that
//
//   * the offsets of everything after it stay valid, which is what lets a
//     finding quote the real source text and name the real line, and
//   * the word counter sees it exactly once.
//
// `xref:`/link macros are masked around their bracketed text instead: a reader
// sees the link text, so the link text is what gets counted.
//
// Output
// ------
// Two rule keys, because C2 is hard in API docs and soft in essays — see
// ADVISORY_DIRS below. `C2` is the hard slice and the one a gate binds;
// `advisory-C2` is the design essays, measured and reported but never blocking.
// A third key, `BACKTICK`, reports blocks whose inline code spans are unbalanced.
//
// Usage: node doc/lint/sentence-length.mjs [--max N] [corpusDir ...]
//   Corpora default to `modules` (the Antora pages) and `lint/.docstrings` (the
//   header docstrings, produced by extract-docstrings.mjs), both relative to
//   doc/. A missing or empty corpus is a HARD ERROR (exit 1), never a clean
//   zero: this branch has eight recorded instances of a check that looked
//   healthy while checking less than it appeared to, and "the corpus silently
//   scanned no files" is the cheapest way to add a ninth.
//
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DOC_DIR = path.resolve(SCRIPT_DIR, '..');
const DEFAULT_CORPORA = ['modules', 'lint/.docstrings'];

const argv = process.argv.slice(2);
let max = 25;
const corpora = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--max') max = Number(argv[++i]);
  else if (argv[i].startsWith('--max=')) max = Number(argv[i].slice('--max='.length));
  else corpora.push(argv[i]);
}
if (!Number.isInteger(max) || max < 1) {
  console.error(`--max expects a positive integer, got: ${max}`);
  process.exit(2);
}
if (corpora.length === 0) corpora.push(...DEFAULT_CORPORA);

function walk(dir) {
  let out = [];
  for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, ent.name);
    if (ent.isDirectory()) out = out.concat(walk(p));
    else if (ent.name.endsWith('.adoc')) out.push(p);
  }
  return out;
}

// --- prose extraction ------------------------------------------------------
//
// Delimited blocks whose content is NOT prose. Their content is skipped
// wholesale — handing code to a prose linter is the `BlockIgnores` mistake
// .vale.ini documents at length, and it is not repeated here. Note that
// `====` (example), `____` (quote) and `****` (sidebar) blocks DO hold prose
// and are only unit boundaries; `--` open blocks do not occur in this corpus.
const OPAQUE_OPEN = /^(-{4,}|\.{4,}|\+{4,}|\/{4,})$/;
const TABLE_DELIM = /^[|,!]===$/;

// Lines that carry no prose of their own. Headings are included: a heading is
// not a sentence, and the longest in the corpus is nowhere near 25 words.
const SKIP_LINE = new RegExp([
  '^//',                              // line comment
  '^:[^\\s:]+:',                      // document attribute
  '^\\[.*\\]$',                       // block attribute, or [[anchor]]
  '^(include|ifdef|ifndef|ifeval|endif|image|video|audio|toc)::',
  '^=+\\s',                           // heading
  '^(={4,}|_{4,}|\\*{4,}|\'{3,})$',   // prose-block delimiters and thematic break
  '^\\+$',                            // list continuation
].join('|'));

// A leading list, callout or ordered-list marker. The item after it is its own
// block to asciidoctor, hence its own sentence scope.
const LIST_MARKER = /^(?:[*.]{1,5}|-|\d+\.|<\d+>|<\.>)\s+/;

// Collect the prose blocks of one file as { line, text }, where `line` is the
// 1-based line the block starts on and `text` is its lines joined with a single
// space. `map` records where each source line begins inside `text`, so a
// sentence found mid-block can still be reported against the line it starts on.
function proseBlocks(text) {
  const lines = text.split('\n');
  const out = [];
  let cur = null;
  let opaque = null; // RegExp that closes the opaque block we are inside
  let inTable = false;

  const open = (lineNo, s) => { cur = { line: lineNo, text: s, map: [{ at: 0, line: lineNo }] }; };
  const append = (lineNo, s) => {
    if (cur === null) return open(lineNo, s);
    cur.text += ' ';
    cur.map.push({ at: cur.text.length, line: lineNo });
    cur.text += s;
  };
  const flush = () => { if (cur && cur.text.trim()) out.push(cur); cur = null; };

  for (let i = 0; i < lines.length; i++) {
    const lineNo = i + 1;
    const t = lines[i].trim();
    if (opaque !== null) { if (opaque.test(t)) opaque = null; continue; }
    if (t === '') { flush(); continue; }
    if (TABLE_DELIM.test(t)) { flush(); inTable = !inTable; continue; }
    if (OPAQUE_OPEN.test(t)) {
      flush();
      // Close on the same delimiter character; asciidoctor wants matching
      // lengths, but being lenient here cannot swallow the rest of the file.
      opaque = new RegExp(`^\\${t[0]}{4,}$`);
      continue;
    }
    if (SKIP_LINE.test(t)) { flush(); continue; }
    if (inTable && t.startsWith('|')) {
      // Every `|` on the row opens a cell, and a cell is its own prose block.
      // The last cell stays open so a cell continued on the next line folds in.
      flush();
      const cells = t.split('|').slice(1);
      for (let k = 0; k < cells.length; k++) {
        const cell = cells[k].trim();
        if (!cell) continue;
        if (k === cells.length - 1) open(lineNo, cell);
        else out.push({ line: lineNo, text: cell, map: [{ at: 0, line: lineNo }] });
      }
      continue;
    }
    const marker = LIST_MARKER.exec(t);
    if (marker) { flush(); open(lineNo, t.slice(marker[0].length)); continue; }
    append(lineNo, t);
  }
  flush();
  return out;
}

// --- masking ---------------------------------------------------------------
//
// Each rule replaces its match with a same-length string. `keep: false` leaves
// one `x` (the single word a reader sees); `keep: true` leaves capture group 1
// (a link's visible text) and hides the rest.
//
// Order matters: the macros run first so that a macro's target — which may
// itself contain brackets, colons or backticks — is consumed as one unit rather
// than being carved up by a later rule, and an already-masked region cannot
// re-match because U+0001 appears in none of the patterns. Note that `keep: true`
// deliberately PRESERVES the link text, including any backticks in it, so those
// backticks stay visible to the backtick rule and to the unbalanced-backtick
// guard below. That is intended — link text is prose — and it is also how a
// backtick can pair across constructs, which is what the guard catches.
const SPANS = [
  { re: /\b(?:xref|link|kbd|btn|menu|footnote):[^\s[]*\[([^\]]*)\]/g, keep: true },
  { re: /\bhttps?:\/\/\S*?\[([^\]]*)\]/g, keep: true },
  { re: /\b(?:cpp|image|icon|pass):[^\s[]*\[[^\]]*\]/g, keep: false },
  { re: /``[^`]+``|`[^`\n]+`/g, keep: false, id: 'backtick' },
  { re: /\{[a-z][\w-]*\}/g, keep: false }, // attribute reference, e.g. {cpp}
];

const hide = (n) => ''.repeat(n);

function mask(s, skip = null) {
  let out = s;
  for (const { re, keep, id } of SPANS) {
    if (id !== undefined && id === skip) continue;
    out = out.replace(re, (m, g1) => {
      if (keep && g1) {
        const at = m.indexOf(g1);
        return hide(at) + g1 + hide(m.length - at - g1.length);
      }
      return `x${hide(m.length - 1)}`;
    });
  }
  return out;
}

// A residual backtick in the masked text means the block held an UNBALANCED
// inline code span, and the mask has already done damage: the stray backtick
// pairs with an unrelated one and, because the mask preserves length, every word
// between them collapses into a single `x`. Measured on a fixture, one stray
// backtick turns a 30-word sentence into 6 — silent UNDER-reporting in a check
// meant to feed a merge-blocking gate, i.e. the exact failure shape this script
// exists to remove. So it is made visible instead: the block is re-masked with
// the backtick rule disabled, which counts the span text as full prose
// (over-reporting, the safe direction), and a `BACKTICK` finding names the file
// and line. The rule's own `\n` guard is not enough because proseBlocks() joins a
// block's lines with a space, so a stray backtick on one line can reach a
// backtick on another. `BACKTICK` findings fingerprint as
// `BACKTICK:file:#N:message`, which does NOT match a `^C2:` gate spec, so they
// are visible without being blocking.
function maskBlock(text) {
  const masked = mask(text);
  if (!masked.includes('`')) return { masked, unbalanced: false };
  return { masked: mask(text, 'backtick'), unbalanced: true };
}

// --- segmentation ----------------------------------------------------------
//
// Terminal punctuation followed by whitespace or end of block, on the MASKED
// text — so a `.` inside a code span or a URL cannot open a sentence boundary.
//
// The inline-formatting marks are part of the boundary, not after it: AsciiDoc's
// bold run-in lead (`*The library owns the handles.* Corosio creates ...`) and the
// same idiom in docstrings (`... `run_async(ex)(task)`.** The wrapper's ...`)
// put `*` or `_` between the period and the space. Requiring whitespace
// immediately after the period merged those leads into the following sentence and
// over-reported its length — two confirmed false positives.
//
// The two UNDER-reporting cases, both fixed here. Every other known miscount in
// this script over-reports, which is the safe direction for a length limit; these
// two let a real violation through, which a merge-blocking gate cannot afford.
// Both were pre-existing (the pre-round code splits identically), and both were
// found by adversarial fixtures rather than by the corpus.
//
//   * A mid-sentence ELLIPSIS. `...` ends with a period followed by a space, so a
//     34-word sentence containing one was segmented 16 + 18 and missed at 25. The
//     `(?<!\.\.)` guard means the third dot of `...` is not a boundary, and the
//     first two are already excluded by the whitespace requirement — so an
//     ellipsis produces no boundary at all. A sentence genuinely ENDING in `...`
//     therefore merges into the next one, which over-reports. Safe direction.
//   * A PARENTHESISED abbreviation, `(e.g.)`. The boundary match consumes the
//     closing bracket, so the slice handed to ABBREV ended `.)` and the
//     abbreviation test — anchored on `\.$` — could not fire. ABBREV now tolerates
//     the same trailing bracket/quote/formatting run the boundary consumes.
//
// There is deliberately NO general "single capital letter plus period is an
// initial" rule: this corpus has no personal initials, but it does end sentences
// on template parameter names ("... can vary from 0 to N. It provides ..."), and
// such a rule merged those into the next sentence.
const TRAILING = '[)"\'’”]*[*_`#]*';
const BOUNDARY = new RegExp(`(?<!\\.\\.)[.!?]${TRAILING}(?=\\s|$)`, 'g');
const ABBREV = new RegExp(`\\b(?:e\\.g|i\\.e|etc|vs|cf|al|resp|approx|Dr|Mr|Mrs|Ms|Fig|No|Ch|Sec|Eq)\\.${TRAILING}$`);

function sentenceRanges(masked) {
  const out = [];
  let start = 0;
  let m;
  BOUNDARY.lastIndex = 0;
  while ((m = BOUNDARY.exec(masked)) !== null) {
    const end = m.index + m[0].length;
    if (ABBREV.test(masked.slice(start, end))) continue;
    out.push([start, end]);
    start = end;
  }
  if (masked.slice(start).trim()) out.push([start, masked.length]);
  return out;
}

// Words as a READER counts them (maintainer ruling). The retired Vale rule's
// token was `\b(\w+)\b`, which splits every hyphenated compound, possessive,
// contraction, slashed run, dotted form and qualified identifier: `most-derived`,
// `fine-grained`, `caller's`, `don't`, `I/O`, `read/write/seek`, `e.g.`, `0.8.0`,
// `buffer_array.hpp` and `this_coro::executor_tag` each counted 2 or more where a
// reader counts 1. Inheriting that token over-counted dozens of findings past the
// limit — findings that are not violations and must not be handed to a wording
// task. So `-`, `'`/`’`, `/`, `.` and `::` are word-internal when they join two
// word characters, and they chain, so a slashed LIST counts one word too.
//
// This is word counting ONLY. It cannot affect where a sentence starts or ends:
// segmentation runs first (see sentenceRanges) and this counter only ever sees an
// already-delimited slice, so the `.` connector can never reach across a sentence
// boundary. The boundary side of dotted forms is ABBREV's job.
const WORD = /\w+(?:(?:[-'’/.]|::)\w+)*/g;
const countWords = (s) => (s.match(WORD) || []).length;

// --- hard versus advisory slice (maintainer ruling) -------------------------
//
// doc/STYLE_GUIDE.md Part C2 makes the limit "hard in API docs, soft in essays".
// The flat 25 stays — a 20-word instruction limit was rejected as unimplementable,
// since nothing classifies instruction-versus-descriptive prose reliably — but the
// OUTPUT is split so a gate can bind only the hard part:
//
//   hard      the extracted `include/**` docstrings, plus every .adoc page NOT in
//             the two essay directories below. This is the number that must reach
//             zero, and the slice a `--gate 'sentence_length:^C2:'` spec binds.
//   advisory  doc/modules/ROOT/pages/2.networking-tutorial/. Measured and
//             reported, never blocking. That chapter is essay-style prose
//             teaching protocol theory, which is exactly the material C2
//             relaxes for, and it holds 72 of the 143 page-level hits measured
//             at the port -- so gating it would make the hard slice mostly
//             essays. Docstrings are always hard, whatever directory they came
//             from.
//
// The advisory rule key deliberately does NOT begin with `C2`, so that even a
// mis-written head-anchored spec (`^C2` without the colon) cannot reach the
// essays. Keep it that way.
const ADVISORY_DIRS = [
  'modules/ROOT/pages/2.networking-tutorial/',
];
// Matched as a whole path SEGMENT sequence, with the trailing slash, so a
// look-alike directory (`2.networking-tutorialish/`) stays in the hard slice. The `/`-prefixed
// form is also tested because an ad-hoc invocation with an absolute corpus path
// outside doc/ makes `rel` a `../`-walk-up, which no prefix test would match —
// that silently put every essay finding in the HARD slice. baseline.mjs always
// passes the relative defaults, so this only ever affected manual runs.
const ruleFor = (rel) => (ADVISORY_DIRS.some((d) => rel.startsWith(d) || rel.includes(`/${d}`))
  ? 'advisory-C2' : 'C2');

// --- run -------------------------------------------------------------------

const byRule = { C2: [], 'advisory-C2': [], BACKTICK: [] };
const scanned = {};
for (const corpus of corpora) {
  const root = path.resolve(DOC_DIR, corpus);
  if (!fs.existsSync(root)) {
    console.error(`sentence-length.mjs: corpus '${corpus}' does not exist at ${root}. ` +
      "For 'lint/.docstrings', run `node lint/extract-docstrings.mjs` first. " +
      'Refusing to report zero findings for a corpus that was never read.');
    process.exit(1);
  }
  const files = walk(root);
  if (files.length === 0) {
    console.error(`sentence-length.mjs: corpus '${corpus}' contains no .adoc files. ` +
      'Refusing to report zero findings for an empty corpus.');
    process.exit(1);
  }
  scanned[corpus] = files.length;
  for (const file of files) {
    // Keyed on the path relative to DOC_DIR, never a basename: `concept/read_stream.hpp`
    // and `test/read_stream.hpp` are different files, and two verification scripts on
    // this branch silently conflated exactly that pair.
    const rel = path.relative(DOC_DIR, file).split(path.sep).join('/');
    const rule = ruleFor(rel);
    for (const block of proseBlocks(fs.readFileSync(file, 'utf8'))) {
      const { masked, unbalanced } = maskBlock(block.text);
      const lineOf = (at) => {
        let line = block.line;
        for (const e of block.map) if (e.at <= at) line = e.line;
        return line;
      };
      if (unbalanced) {
        byRule.BACKTICK.push({
          file: rel,
          line: block.line,
          message: 'unbalanced backtick in block; inline code spans in it are counted as prose',
          sentence: block.text.slice(0, 200).trim(),
        });
      }
      for (const [from, to] of sentenceRanges(masked)) {
        const words = countWords(masked.slice(from, to));
        if (words <= max) continue;
        byRule[rule].push({
          file: rel,
          line: lineOf(from),
          words,
          // The message is deliberately fixed text: baseline.mjs fingerprints a
          // doc_lint-shaped finding as `rule:file:#N:message`, and folding the
          // word count or the sentence into it would re-mint the fingerprint on
          // every reword — a gate that fails because a contributor rephrased an
          // already-over-limit sentence teaches contributors to distrust it.
          message: `sentence over ${max} words`,
          sentence: block.text.slice(from, to).trim(),
        });
      }
    }
  }
}

console.log(JSON.stringify({
  summary: {
    hard: byRule.C2.length,
    advisory: byRule['advisory-C2'].length,
    unbalancedBackticks: byRule.BACKTICK.length,
    max,
    scanned,
    advisoryDirs: ADVISORY_DIRS,
  },
  findings: byRule,
}, null, 2));
process.exit(0);
