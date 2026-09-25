#!/usr/bin/env node
//
// baseline.mjs — runs every check and snapshots current violations to
// doc/lint/baseline.json (Style Guide Part F.0, "no new violations" while the
// backlog is worked down). Node built-ins only, no dependencies.
//
// Each check contributes a `count` and a `fingerprints` array (stable
// per-finding strings) so a later comparator (check-no-new-violations.mjs)
// can diff a fresh run against this snapshot and flag genuinely new
// findings, independent of how many pre-existing ones remain. Fingerprints
// deliberately carry no line number — see occurrenceKey() below.
//
// Usage: node doc/lint/baseline.mjs [outFile]
//   outFile defaults to doc/lint/baseline.json; check-no-new-violations.mjs
//   passes a temp path so a comparison run doesn't clobber the committed one.
//
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DOC_DIR = path.resolve(SCRIPT_DIR, '..');
const REPO_ROOT = path.resolve(DOC_DIR, '..');
const cliArgs = process.argv.slice(2);
// --details additionally emits a `details` map per check, keyed by the SAME
// fingerprint string, carrying { file, line, excerpt } for human-readable
// reporting. Deliberately opt-in and deliberately NOT part of a committed
// baseline: line numbers move, so persisting them would reintroduce exactly
// the churn occurrenceKey() exists to avoid (see its comment). The reseed
// path never passes this; check-no-new-violations.mjs always does.
const wantDetails = cliArgs.includes('--details');
const outArg = cliArgs.find((a) => !a.startsWith('--'));

// Vale does not parse AsciiDoc itself: it shells out to `asciidoctor` and lints
// the HTML. With none on PATH it exits 2 with a runtime error, valeFingerprints()
// reports the check SKIPPED, and check-no-new-violations.mjs still exits 0 — a
// local run then passes while linting no prose at all.
//
// `asciidoctor` is a devDependency of doc/package.json, added for exactly this, and
// `npm ci` in doc/build_antora.sh installs it at doc/node_modules/.bin/asciidoctor.
// Antora does NOT pull it in on its own -- @asciidoctor/core provides no binary --
// so do not prune that devDependency: without it both Vale checks skip, which is
// the fail-open this block exists to prevent. It is APPENDED, not prepended: CI
// apt-installs the Ruby asciidoctor, the committed baseline is authored against
// that one, and the two produce different HTML and therefore different findings
// (the drift the reseed table records). Whatever PATH already offers keeps winning.
const VALE_PATH = [
  process.env.PATH || '',
  path.join(DOC_DIR, 'node_modules', '.bin'),
].filter(Boolean).join(path.delimiter);

function run(cmd, args, opts = {}) {
  const env = { ...process.env, PATH: VALE_PATH, ...(opts.env || {}) };
  const r = spawnSync(cmd, args, {
    encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, ...opts, env,
  });
  return r;
}

// Fingerprints must survive line shifts. Keying on the reported line number made
// every finding BELOW an insertion point look new: commit df68f9bc, a comment-only
// docstring addition, renamed 27 grandfathered MrDocs findings and red-lined the
// blocking MrDocs-no-warnings gate without introducing a single warning. So the
// line number is replaced by a per-group occurrence index: the Nth finding sharing
// the same (head, tail) pair is keyed `#N`. That keeps multiplicity — the same
// warning appearing one MORE time in the same file is still new — while making the
// key independent of where in the file it appears.
//
// The index goes exactly where the line number was, mid-key. Do not move it to the
// tail: .github/workflows/docs.yml gates on `doc_lint:^(A1|A6|B2|D2):` (head-anchored)
// and `vale_adoc:Corosio\.PartHeadings$` (TAIL-anchored), and the regexes are tested
// against the whole fingerprint, so a trailing index would make the PartHeadings
// gate match nothing and fail open.
//
// The counter is per-base-key, never a raw iteration counter, so the resulting key
// multiset is {base:#1 .. base:#k} whatever order the findings arrive in.
function occurrenceKey(seen, head, tail) {
  const base = `${head}\u0000${tail}`; // NUL separator: neither part can contain it
  const n = (seen.get(base) ?? 0) + 1;
  seen.set(base, n);
  return `${head}:#${n}:${tail}`;
}

// Vale is spawned below with cwd=DOC_DIR, so the keys of its JSON output are
// paths relative to DOC_DIR (or absolute). `path.relative(DOC_DIR, file)`
// resolved a *relative* `file` against process.cwd(), NOT against DOC_DIR — so
// regenerating from anywhere other than doc/ prefixed every Vale path with
// `../` and silently renamed all ~3900 Vale fingerprints at once, retiring the
// entire grandfathered Vale backlog and re-minting it under new keys. Resolve
// against DOC_DIR explicitly so the key depends only on the file, never on
// where the generator happened to be invoked from. The separator normalisation
// is a no-op on POSIX (path.sep === '/') and keeps a Windows run from minting a
// parallel backslash-keyed key set.
function valeRelPath(file) {
  return path.relative(DOC_DIR, path.resolve(DOC_DIR, file)).split(path.sep).join('/');
}


// ---------------------------------------------------------------------------
// Source resolution for human-readable reporting (--details only).
//
// A docstring finding is reported against lint/.docstrings/<h>.adoc, a
// GENERATED file nobody edits — unactionable on its own. extract-docstrings.mjs
// writes a `<file>.lines.json` sidecar mapping output-line ranges back to the
// line in the real header where that doc comment starts; we then refine within
// the block by locating the excerpt in the header text, because a single doc
// comment can be 100 output lines long.
const lineMapCache = new Map();
const srcCache = new Map();

const normText = (s) => s.replace(/^[\s*\/]+/, '').replace(/\s+/g, ' ').trim();

// Find `excerpt` in the header at/after `fromLine`, tolerating the reflow that
// cleanBlock() applied (`*` prefixes stripped, continuation lines folded). Returns
// a 1-based line, or null when the probe is too short or does not match — in which
// case the caller keeps the block's start line, which is always correct if coarse.
function locateInSource(relSource, fromLine, excerpt) {
  if (!excerpt) return null;
  let lines = srcCache.get(relSource);
  if (lines === undefined) {
    try { lines = fs.readFileSync(path.join(REPO_ROOT, relSource), 'utf8').split('\n'); }
    catch { lines = null; }
    srcCache.set(relSource, lines);
  }
  if (!lines) return null;
  let buf = '';
  const owner = [];
  for (let i = fromLine - 1; i < Math.min(lines.length, fromLine + 400); i++) {
    const t = normText(lines[i]);
    if (!t) continue;
    if (buf) { buf += ' '; owner.push(i + 1); }
    for (let k = 0; k < t.length; k++) owner.push(i + 1);
    buf += t;
  }
  const probe = normText(excerpt).slice(0, 60);
  if (probe.length < 12) return null;
  const at = buf.indexOf(probe);
  return at >= 0 ? owner[at] : null;
}

// Map a linted path + line to the file a developer should actually open.
function resolveSource(relFile, line, excerpt) {
  if (!relFile || !relFile.startsWith('lint/.docstrings/')) {
    return { file: relFile ? `doc/${relFile}` : null, line: line ?? null };
  }
  const mapPath = path.join(DOC_DIR, `${relFile}.lines.json`);
  let map = lineMapCache.get(mapPath);
  if (map === undefined) {
    try { map = JSON.parse(fs.readFileSync(mapPath, 'utf8')); } catch { map = null; }
    lineMapCache.set(mapPath, map);
  }
  if (!map) return { file: `doc/${relFile}`, line: line ?? null };
  const span = line == null ? null : map.spans.find((sp) => line >= sp.from && line <= sp.to);
  if (!span) return { file: map.source, line: null };
  return { file: map.source, line: locateInSource(map.source, span.srcLine, excerpt) ?? span.srcLine };
}

// Head+tail excerpt: a 30-word sentence is unreadable inline, but its opening
// and closing words are what let you find it in the file.
function excerptOf(text, max = 96) {
  if (!text) return null;
  const t = normText(String(text));
  if (t.length <= max) return t;
  return `${t.slice(0, max - 28).trimEnd()} ... ${t.slice(-22).trimStart()}`;
}

function valeFingerprints(target) {
  const r = run('vale', ['--output=JSON', target], { cwd: DOC_DIR });
  if (r.error) {
    return { count: 0, skipped: true, reason: `vale failed to launch: ${r.error.message}`, fingerprints: [] };
  }
  // Vale's own exit codes: 0 = no alerts at MinAlertLevel, 1 = alerts found (the normal,
  // expected case — NOT a failure), 2 = fatal runtime error (e.g. `asciidoctor` off PATH,
  // a broken `vale sync`). On a fatal error Vale writes a single JSON error object to
  // stderr and leaves stdout empty, so a plain JSON.parse(stdout || '{}') silently yields
  // `{}` — indistinguishable from "ran clean, found nothing." Detect that explicitly
  // instead of ever reporting a broken Vale run as `count: 0`.
  let parsed = null;
  try { parsed = r.stdout ? JSON.parse(r.stdout) : null; } catch { parsed = null; }
  const looksLikeFileMap = parsed !== null && typeof parsed === 'object' && !Array.isArray(parsed);
  if (r.status === 2 || !looksLikeFileMap) {
    const tail = (r.stderr || r.stdout || '(no output)').trim().slice(-500);
    return { count: 0, skipped: true, reason: `vale on '${target}' did not produce findings (exit ${r.status}): ${tail}`, fingerprints: [] };
  }
  const fingerprints = [];
  const details = {};
  const seen = new Map();
  for (const [file, alerts] of Object.entries(parsed)) {
    const rel = valeRelPath(file);
    for (const a of alerts) {
      const fp = occurrenceKey(seen, rel, a.Check);
      fingerprints.push(fp);
      if (wantDetails) {
        details[fp] = { ...resolveSource(rel, a.Line, a.Match), rule: a.Check,
                        message: a.Message, excerpt: excerptOf(a.Match) };
      }
    }
  }
  return { count: fingerprints.length, fingerprints: fingerprints.sort(), details };
}

// A crashed check must report as SKIPPED, never as zero findings. doc-lint.mjs and
// mrdocs-warnings.mjs have no error path for an *uncaught throw*: they die with a
// non-zero status and an empty stdout. Defaulting that to an empty findings object
// yielded `count: 0, skipped: false` — a crashed check indistinguishable from a
// clean one. Both are GATED, and the comparator only treats a `skipped` check as
// unverifiable, so the zero sailed through as "backlog empty": the reseed reporter
// called it "0 added, none gated" and the merge gate called it "0 new". That is the
// fail-open shape this toolchain exists to prevent, so the exit status is now
// checked before the output is believed. (mrdocs-warnings.mjs's *designed*
// failures — no binary, version-pin miss, MrDocs itself failing — already emit
// `{error}` on exit 0 and are handled below; this only covers crashes.)
function crashed(r, script) {
  if (!r.error && r.status === 0) return null;
  const tail = (r.stderr || r.error?.message || r.stdout || '(no output)').trim().slice(-500);
  return { count: 0, skipped: true, reason: `${script} failed (exit ${r.status}): ${tail}`, fingerprints: [] };
}

function docLintFingerprints() {
  const r = run('node', [path.join(SCRIPT_DIR, 'doc-lint.mjs')]);
  const bad = crashed(r, 'doc-lint.mjs');
  if (bad) return bad;
  let parsed;
  try {
    parsed = JSON.parse(r.stdout || '');
  } catch {
    return {
      count: 0, skipped: true, fingerprints: [],
      reason: `doc-lint.mjs produced unparseable output: ${(r.stdout || '(empty)').trim().slice(-500)}`,
    };
  }
  const fingerprints = [];
  const details = {};
  const seen = new Map();
  for (const [check, items] of Object.entries(parsed.findings || {})) {
    // SHAPE is advisory-only and never gated (see doc-lint.mjs's header
    // comment); folding it into doc_lint's fingerprint set let a single
    // advisory SHAPE finding keep `currentSet.length` non-zero in
    // check-no-new-violations.mjs even when A1/A6/B2/D2 — the checks the
    // gate spec `doc_lint:^(A1|A6|B2|D2):` actually cares about — report
    // zero, silently disarming the "gated check reports 0 against a
    // non-empty baseline" backstop (check-no-new-violations.mjs:187).
    if (check === 'SHAPE') continue;
    for (const it of items) {
      const fp = occurrenceKey(seen, `${check}:${it.file}`, it.message);
      fingerprints.push(fp);
      // doc-lint reports paths relative to the pages tree, not to doc/.
      if (wantDetails) {
        details[fp] = { ...resolveSource(`modules/ROOT/pages/${it.file}`, it.line ?? null, null),
                        rule: check, message: it.message, excerpt: null };
      }
    }
  }
  return { count: fingerprints.length, byRule: parsed.summary, fingerprints: fingerprints.sort(), details };
}

// C2 (sentence length) is checked by our own script, not by Vale — see the
// header of sentence-length.mjs and the comment in Corosio/SentenceLength.yml for
// why. The finding shape is doc-lint.mjs's, so the fingerprint is the doc_lint
// shape (`rule:file:#N:message`, rule at the HEAD) and a future gate spec reads
// `--gate 'sentence_length:^C2:'`. Note this check has no entry in the
// committed baseline.json yet, so every finding reports as NEW until the
// maintainer reseeds; it is deliberately NOT in the gate spec, so that cannot
// block a merge.
function sentenceLengthFingerprints() {
  const r = run('node', [path.join(SCRIPT_DIR, 'sentence-length.mjs')], { cwd: DOC_DIR });
  const bad = crashed(r, 'sentence-length.mjs');
  if (bad) return bad;
  let parsed;
  try {
    parsed = JSON.parse(r.stdout || '');
  } catch {
    return {
      count: 0, skipped: true, fingerprints: [],
      reason: `sentence-length.mjs produced unparseable output: ${(r.stdout || '(empty)').trim().slice(-500)}`,
    };
  }
  const fingerprints = [];
  const details = {};
  const seen = new Map();
  for (const [check, items] of Object.entries(parsed.findings || {})) {
    for (const it of items) {
      const fp = occurrenceKey(seen, `${check}:${it.file}`, it.message);
      fingerprints.push(fp);
      if (wantDetails) {
        details[fp] = { ...resolveSource(it.file, it.line ?? null, it.sentence),
                        rule: check, message: it.words ? `${it.message} (${it.words})` : it.message,
                        excerpt: excerptOf(it.sentence) };
      }
    }
  }
  return { count: fingerprints.length, byRule: parsed.summary, fingerprints: fingerprints.sort(), details };
}

function mrdocsFingerprints() {
  const r = run('node', [path.join(SCRIPT_DIR, 'mrdocs-warnings.mjs')]);
  const bad = crashed(r, 'mrdocs-warnings.mjs');
  if (bad) return bad;
  let parsed = {};
  try { parsed = JSON.parse(r.stdout || '{}'); } catch { /* fall through */ }
  if (parsed.error) return { count: 0, skipped: true, reason: parsed.error, fingerprints: [] };
  const seen = new Map();
  const details = {};
  const fingerprints = (parsed.findings || []).map((f) => {
    const fp = occurrenceKey(seen, f.file ?? '?', f.message);
    if (wantDetails) {
      details[fp] = { file: f.file ?? null, line: f.line ?? null,
                      rule: 'mrdocs', message: f.message, excerpt: null };
    }
    return fp;
  });
  return { count: fingerprints.length, fingerprints: fingerprints.sort(), details };
}

const results = {};
results.vale_adoc = valeFingerprints('modules');

// The docstring corpus is GENERATED, so the generator's exit status is part of
// the measurement. It used to be discarded: extract-docstrings.mjs never clears
// OUT_DIR, so a crash left whatever the last successful run wrote — a stale
// corpus that Vale lints happily and reports `skipped: false` over. Worse, if
// the crash happened before anything was ever written (fresh clone, renamed
// path), Vale over an absent directory exits 0 with `{}`, which valeFingerprints
// reads as `count: 0, skipped: false` — a vacuous clean for the C4/C9/C10
// docstring gates and for sentence_length's docstring half. Both dependent
// checks are therefore marked SKIPPED, which the comparator treats as a gate
// failure, instead of being believed.
const extract = run('node', [path.join(SCRIPT_DIR, 'extract-docstrings.mjs')]);
const extractBad = crashed(extract, 'extract-docstrings.mjs');
if (extractBad) {
  const reason = `docstring corpus not regenerated: ${extractBad.reason}`;
  results.vale_docstrings = { count: 0, skipped: true, reason, fingerprints: [] };
  results.sentence_length = { count: 0, skipped: true, reason, fingerprints: [] };
} else {
  results.vale_docstrings = valeFingerprints('lint/.docstrings');

  // After extract-docstrings.mjs above: sentence-length.mjs lints BOTH corpora and
  // exits non-zero rather than reporting a clean zero for one it could not read.
  results.sentence_length = sentenceLengthFingerprints();
}

results.doc_lint = docLintFingerprints();
results.mrdocs_warnings = mrdocsFingerprints();
// No a11y check: E4 is Review tier (doc/STYLE_GUIDE.md Part F.0) and a scan that
// cannot fail CI is not carried. run-a11y.mjs and pa11y-ci are deliberately not
// ported from Capy -- see doc/design/style-guide-compliance.md section 4.4.

const baseline = {
  generatedAt: new Date().toISOString(),
  note: 'Snapshot of current violations (Style Guide Part F.0). Everything recorded ' +
        'here is grandfathered; check-no-new-violations.mjs fails on findings that are ' +
        'NOT in it, for the rules named by the workflow\'s --gate spec. Reseed only via ' +
        'the workflow_dispatch steps, never locally. Fingerprints are line-insensitive: ' +
        'the `#N` component is the Nth occurrence of that (file, message) pair, NOT a ' +
        'line number, so inserting text above a finding does not rename it.',
  checks: Object.fromEntries(Object.entries(results).map(([k, v]) => [k, {
    count: v.count, skipped: v.skipped || false, reason: v.reason,
    byRule: v.byRule, contrastCount: v.contrastCount,
    fingerprints: v.fingerprints,
    ...(wantDetails && v.details ? { details: v.details } : {}),
  }])),
};

const outPath = outArg ? path.resolve(outArg) : path.join(SCRIPT_DIR, 'baseline.json');
fs.writeFileSync(outPath, JSON.stringify(baseline, null, 2) + '\n');
console.log(JSON.stringify({
  written: path.relative(REPO_ROOT, outPath),
  summary: Object.fromEntries(Object.entries(baseline.checks).map(([k, v]) => [k, v.skipped ? 'skipped' : v.count])),
}, null, 2));
