#!/usr/bin/env node
//
// doc-lint.mjs — structural checks Vale cannot express (Style Guide Part F).
// Node built-ins only, no dependencies. Exit 0 always (warning mode, Task 2);
// findings are emitted as JSON on stdout for `baseline.json` / CI to consume.
//
// Checks:
//   A1 — every page under pages/ declares :page-mode:, and the value is one
//        of doc/STYLE_GUIDE.md Part A's four Diátaxis modes (tutorial,
//        how-to, reference, explanation). Presence alone used to pass; a
//        typo or a non-mode value (e.g. the former `concept`) slipped
//        through and silently fell out of D2's scope (see below). Bite-test
//        per style-guide F4: plant an invalid value, confirm A1 fails.
//   A6 — quick-start is within the first 3 top-level nav.adoc entries
//   B2 — no [source,<any-lang>] block, and no bare listing (`----` or
//        `....`), holds raw code (must start with include::example$... or
//        carry role=pseudocode/role=external — doc/STYLE_GUIDE.md B3). A
//        bare listing whose attribute line carries role=output/role=figure
//        is exempt from B2 outright — it is not code, but see SHAPE below,
//        which still looks at it. Originally scoped to [source,cpp]/
//        [source,c++] only, which left [source,cmake]/[source,c]/
//        [source,bash] and bare listings holding real C++ invisible to the
//        gate; widened once every such block in the corpus was classified
//        (see git history for the audit). Delimiters are matched 4-or-more
//        repeats of the character, closer length must equal opener length
//        (AsciiDoc's own rule — `-----`/`....` are not `----`, and this is
//        also how AsciiDoc nests a `----` inside a `-----`); a fixed
//        4-character match let a 5-dash listing hide code from the gate.
//        The attribute list read above the delimiter walks consecutive
//        `[source,...]`/`[role=...]` lines, not just the nearest one —
//        AsciiDoc permits a block's attribute list to be split across
//        adjacent lines (e.g. `[source,cpp]` then `[role=output]` on the
//        next line) and Asciidoctor merges them; reading only the nearest
//        line missed `[source,...]` set on an earlier line and let a
//        highlighted C++ block through as an exempt bare listing. The walk
//        deliberately stops at anything else `[...]`-shaped (a block anchor
//        `[[id]]`, an admonition style `[NOTE]`, a quote attribution
//        `[quote,...]`) — a first cut that merged any `[...]`-shaped line
//        made one of those, sitting directly above an exempt
//        `[source,...,role=pseudocode]` block, defeat that exemption.
//   SHAPE — advisory only, NEVER gated (not in the summary the CI gate
//        spec reads by rule prefix). A role=output/role=figure block is a
//        permanent B2 exemption, so a block wrongly marked non-code would
//        be permanently invisible; SHAPE runs a content heuristic
//        (`#include`, `co_await`, `template<`, a brace-opened struct/class,
//        a `;`-terminated line, `Name::member(`) over exactly the blocks B2
//        just exempted, and flags ones that look like code. The exemption
//        stops being permanent invisibility: the gate still looks, it just
//        doesn't block.
//   ANCHOR — no prose writes a C++ attribute as `[[...]]` inside a code
//        span. Asciidoctor's inline-anchor substitution runs inside a
//        backtick span, so `` `[[nodiscard]]` `` is parsed as an anchor and
//        renders as an EMPTY <code> element -- the attribute name silently
//        disappears from the page. Measured: `[[clang::coro_await_elidable]]`
//        rendered as `<code><a id="clang::coro_await_elidable"></a></code>`.
//        The defect is invisible in the source, which reads correctly, so it
//        needs a machine check rather than a careful reader. The fix is a
//        passthrough: `` `+[[nodiscard]]+` ``. Only
//        prose is scanned -- inside a delimited block `[[` is literal and
//        renders fine, which is why the check is line-based with a
//        block-depth skip rather than a whole-file regex. Bite-test per
//        style-guide F4: put `` `[[nodiscard]]` `` in a page and confirm
//        ANCHOR fires.
//   D2 — every page in a CONCEPT_DIRS chapter (or quick-start.adoc) has
//        >=1 include::example$. D2's "concept page" is a pedagogical
//        category, not a Diátaxis mode — deliberately independent of
//        :page-mode:, so a page cannot leave D2's scope by declaring a
//        different (even a legitimate) mode. See doc/STYLE_GUIDE.md's D2
//        entry for why landing pages (*.intro.adoc, mode: explanation) are
//        outside this scope on purpose, not by omission.
//
import fs from 'node:fs';
import path from 'node:path';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
// An optional CLI arg overrides which pages tree gets walked, so a self-test
// can point this at a throwaway fixture tree instead of the real corpus.
// A6/D2's nav.adoc is unaffected — those checks are orthogonal to what a
// pages-tree override is for (exercising B2 in isolation).
const PAGES_DIR = process.argv[2] ? path.resolve(process.argv[2]) : path.join(ROOT, 'modules/ROOT/pages');
const NAV_FILE = path.join(ROOT, 'modules/ROOT/nav.adoc');

// The four Diátaxis modes doc/STYLE_GUIDE.md Part A defines. A1 rejects
// anything else, including the legacy `concept` value (never a Diátaxis
// mode — it was D2's subject noun leaking into A1's value namespace).
const VALID_MODES = new Set(['tutorial', 'how-to', 'reference', 'explanation']);

// Directories whose pages are concept/tutorial material for D2's heuristic.
// Deliberately NOT keyed off :page-mode: — see the D2 comment above.
// `2.networking-tutorial` is deliberately ABSENT: it teaches IP, TCP and UDP
// theory from first principles and introduces no Corosio type, so there is no
// type to show in an example. It is background material, the same carve-out
// Capy's `3a`-`3d` primer carries. Including it would file ~13 findings that no
// example can fix, and the fix for a D2 finding is never a decorative
// `include::example$` -- see doc/STYLE_GUIDE.md's D2 entry.
const CONCEPT_DIRS = [
  '3.tutorials', '4.guide', '5.testing',
];
const TUTORIAL_FILES = new Set(['quick-start.adoc']);

function walk(dir) {
  let out = [];
  for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, ent.name);
    if (ent.isDirectory()) out = out.concat(walk(p));
    else if (ent.name.endsWith('.adoc')) out.push(p);
  }
  return out;
}

function pageMode(text) {
  const m = text.match(/^:page-mode:\s*(\S+)/m);
  return m ? m[1] : null;
}

function isConceptOrTutorial(relPath) {
  const top = relPath.split(path.sep)[0];
  return TUTORIAL_FILES.has(relPath) || CONCEPT_DIRS.includes(top);
}

// SHAPE's content heuristic (see the header comment). Deliberately narrow —
// this only needs to catch a block that is CLEARLY code, not judge style.
const CODE_SHAPE_PATTERNS = [
  /#include\b/,
  /\bco_await\b/,
  /\btemplate\s*</,
  /\b(?:struct|class)\s+\w+\s*\{/,
  /;\s*$/,
  /\w+::\w+\s*\(/,
];
function looksLikeCode(bodyLines) {
  return bodyLines.some((l) => CODE_SHAPE_PATTERNS.some((re) => re.test(l)));
}

// Walk every block delimited by 4-or-more repeats of `ch` (`-` for
// listing/source blocks, `.` for literal blocks). Returns B2 and SHAPE
// findings (kind-tagged; the caller routes them to the right bucket).
function scanBlocks(lines, ch) {
  const delim = new RegExp(`^\\${ch}{4,}$`);
  const out = [];
  let openLen = null;
  for (let i = 0; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (!delim.test(trimmed)) continue;
    if (openLen !== null) {
      // The closer must repeat `ch` exactly as many times as the opener did;
      // a mismatched length is content (or a nested delimiter of a
      // different length) and does not close this block.
      if (trimmed.length === openLen) openLen = null;
      continue;
    }
    openLen = trimmed.length;
    const openerLine = i;

    // The attribute line(s) immediately above (skipping blank lines before
    // the stack begins), if any. AsciiDoc permits a block's attribute list
    // to be split across multiple adjacent `[...]` lines (no blank line
    // between them) and Asciidoctor merges them into one; reading only the
    // single nearest line missed a role= or [source,...] marker set on an
    // earlier line in the stack, e.g.:
    //   [source,cpp]
    //   [role=output]
    //   ----
    // which used to read attr as just `[role=output]`, miss isSource, and
    // fall into the bare-listing branch below instead of B2. Walk upward
    // collecting consecutive lines, but ONLY ones that look like a
    // continuation of THIS block's attribute list -- `[source,...]` or
    // `[role=...]`, the only two shapes isSource/hasClearingRole/
    // hasNonCodeRole below ever inspect. A bare `/^\[.*\]$/` walk is too
    // wide: a block anchor (`[[id]]`), an admonition style (`[NOTE]`), or a
    // quote attribution (`[quote,...]`) can legitimately sit directly above
    // a block with no blank line between, and merging one of those ahead of
    // a real `[source,cpp,role=pseudocode]` line made the joined string no
    // longer start with `[source`, wrongly flagging an exempt block as B2.
    // Stopping the walk at the first non-source/non-role line excludes them.
    const ATTR_CONTINUATION = /^\[(?:source\b|role=)/i;
    let a = openerLine - 1;
    while (a >= 0 && lines[a].trim() === '') a--;
    const attrLineIdxs = [];
    while (a >= 0 && ATTR_CONTINUATION.test(lines[a].trim())) {
      attrLineIdxs.unshift(a);
      a--;
    }
    const attr = attrLineIdxs.map((idx) => lines[idx].trim()).join(' ');
    const attrTopLine = attrLineIdxs.length ? attrLineIdxs[0] : openerLine;
    const isSource = /^\[source\s*,\s*[^,\]]+/i.test(attr);
    const hasClearingRole = /role=(pseudocode|external)\b/.test(attr);
    const hasNonCodeRole = /role=(output|figure)\b/.test(attr);

    // A [source,<lang>,role=pseudocode|external] block: not a B2 candidate.
    // NB this is deliberately isSource-gated — role=output/role=figure must
    // NOT clear a [source,*] block (that would make role=output a blanket
    // exemption for real code); only pseudocode/external do that, and only
    // on a [source,*] block.
    if (isSource && hasClearingRole) continue;

    if (!isSource && hasNonCodeRole) {
      // Bare listing explicitly marked as program output / a figure: not a
      // B2 candidate, but SHAPE still looks at its content (advisory).
      const body = [];
      for (let m = openerLine + 1; m < lines.length; m++) {
        const t = lines[m].trim();
        if (delim.test(t) && t.length === openLen) break;
        body.push(lines[m]);
      }
      if (looksLikeCode(body)) {
        out.push({
          kind: 'SHAPE',
          line: openerLine + 1,
          message: `role=output/role=figure block's content looks like code, not literal output/a figure (advisory, not gated)`,
        });
      }
      continue;
    }

    // Everything else — any [source,<lang>] block without a clearing role,
    // and any bare listing without a role=output/role=figure marker — must
    // open on a compiled include, or it is raw code pasted into the page.
    let k = openerLine + 1;
    while (k < lines.length && lines[k].trim() === '') k++;
    const first = (lines[k] || '').trim();
    if (!first.startsWith('include::example$')) {
      const line = attr !== '' ? attrTopLine + 1 : openerLine + 1;
      const message = isSource
        ? 'raw code, not include::example$/role=pseudocode/role=external'
        : 'raw code in a bare listing, not include::example$/role=output/role=figure — this block must not contain code';
      out.push({ kind: 'B2', line, message });
    }
  }
  return out;
}

// Prose lines only: a `[[...]]` inside a delimited block is literal and safe.
// Tracks delimiter depth the same way scanBlocks does (4-or-more repeats, the
// closer must match the opener's length) so a `----` nested in a `-----` does
// not end the outer block early and expose its body to the scan.
function scanAnchors(lines) {
  const out = [];
  let openLen = null;
  let openCh = null;
  for (let i = 0; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    const d = /^([-.=_*+])\1{3,}$/.exec(trimmed);
    if (d) {
      if (openLen === null) {
        openLen = trimmed.length;
        openCh = d[1];
      } else if (d[1] === openCh && trimmed.length === openLen) {
        openLen = null;
        openCh = null;
      }
      continue;
    }
    if (openLen !== null) continue;
    if (/`\[\[/.test(lines[i])) {
      out.push({
        line: i + 1,
        message:
          'attribute written as `[[...]]` in a code span renders as an empty <code> ' +
          '(Asciidoctor reads it as an inline anchor); use a passthrough `+[[...]]+`',
      });
    }
  }
  return out;
}

const findings = { A1: [], A6: [], B2: [], SHAPE: [], ANCHOR: [], D2: [] };
const files = walk(PAGES_DIR);

for (const file of files) {
  const rel = path.relative(PAGES_DIR, file);
  const text = fs.readFileSync(file, 'utf8');
  const mode = pageMode(text);

  if (!mode) {
    findings.A1.push({ file: rel, message: 'no :page-mode: attribute' });
  } else if (!VALID_MODES.has(mode)) {
    findings.A1.push({ file: rel, message: `invalid :page-mode: value '${mode}' (must be one of ${[...VALID_MODES].join(', ')})` });
  }

  // Walk every listing (`----`) and literal (`....`) delimited block —
  // source or bare — for B2 and its SHAPE advisory sidecar.
  const lines = text.split('\n');
  for (const b of [...scanBlocks(lines, '-'), ...scanBlocks(lines, '.')]) {
    findings[b.kind].push({ file: rel, line: b.line, message: b.message });
  }

  for (const a of scanAnchors(lines)) {
    findings.ANCHOR.push({ file: rel, line: a.line, message: a.message });
  }

  if (isConceptOrTutorial(rel) && !text.includes('include::example$')) {
    findings.D2.push({ file: rel, message: 'tutorial/concept page has no include::example$' });
  }
}

const navText = fs.readFileSync(NAV_FILE, 'utf8');
const topEntries = navText.split('\n').filter(l => /^\* xref:/.test(l));
const qsIndex = topEntries.findIndex(l => /quick-start/.test(l));
if (qsIndex === -1 || qsIndex > 2) {
  findings.A6.push({ file: 'nav.adoc', message: `quick-start at top-level position ${qsIndex + 1}, must be <= 3` });
}

const summary = Object.fromEntries(Object.entries(findings).map(([k, v]) => [k, v.length]));
console.log(JSON.stringify({ summary, findings }, null, 2));
process.exit(0);
