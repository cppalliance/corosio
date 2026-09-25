#!/usr/bin/env node
//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//
// baseline-diff.mjs — explains what accepting a candidate doc/lint/baseline.json
// would change, so a maintainer reseeding the "no new violations" gate can tell
// "stale entries retired" from "a real new finding absorbed."
//
// Reseeding a baseline REWRITES the gate's reference point: every fingerprint the
// candidate adds becomes grandfathered forever, and every fingerprint it drops
// becomes newly reportable. That is the one operation in this toolchain that can
// silently un-gate a real regression, so it is never automatic — CI produces a
// candidate, this script explains it, and a human commits it. The reseed steps
// live in .github/workflows/docs.yml; the maintainer procedure is in
// doc/lint/README.md.
//
// Node built-ins only, no dependencies.
//
// Usage:
//   node doc/lint/baseline-diff.mjs <committed.json> <candidate.json> \
//        [--gate <check>:<rule-regex> ...] [--examples N]
//
// --gate takes the SAME specs as check-no-new-violations.mjs (split on the first
// ':', regex tested against the whole fingerprint). Pass the live gate spec and
// any added fingerprint that would have blocked a merge is reported separately,
// in full, and as a GitHub error annotation — those are the entries a reseed
// would grandfather away. The CI step derives these specs from the blocking step
// in .github/workflows/docs.yml rather than restating them, so they cannot rot
// apart; see the reseed steps there.
//
// --allow-emptied <check>  acknowledges that a gated check legitimately reached
// zero findings (its backlog is genuinely closed). Repeatable. Without it, a
// gated check that is empty in the candidate while non-empty in the committed
// baseline is FATAL — see emptiedGated below.
//
// Exit status:
//   0  candidate is explainable (it may still add ungated findings — read the report)
//   1  candidate must not be committed as-is. Three reasons, all fail-closed:
//        * a check is `skipped` in it (a skipped check snapshots an empty slice,
//          wiping that slice's grandfathered backlog),
//        * a GATED check collapsed to zero findings without being marked skipped
//          (same wipe, but arrives looking like success — see emptiedGated),
//        * an added fingerprint matches the gate spec (a finding that would have
//          blocked a merge is about to become grandfathered).
// Adding *ungated* findings is not by itself an error: those slices are still
// being worked down, so an intentional new backlog is legitimate. A gated
// addition never is without justification, and neither is an unverifiable check.
//
import fs from 'node:fs';

const argv = process.argv.slice(2);
const gateByCheck = new Map(); // check -> [RegExp]
const allowEmptied = new Set();
const positional = [];
let examples = 5;
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  let spec = null;
  if (a === '--gate') spec = argv[++i];
  else if (a.startsWith('--gate=')) spec = a.slice('--gate='.length);
  else if (a === '--examples') { examples = Number(argv[++i]); continue; }
  else if (a.startsWith('--examples=')) { examples = Number(a.slice('--examples='.length)); continue; }
  else if (a === '--allow-emptied') { allowEmptied.add(argv[++i]); continue; }
  else if (a.startsWith('--allow-emptied=')) { allowEmptied.add(a.slice('--allow-emptied='.length)); continue; }
  else { positional.push(a); continue; }
  const idx = spec.indexOf(':');
  if (idx < 0) {
    console.error(`--gate expects <check>:<rule-regex>, got: ${spec}`);
    process.exit(2);
  }
  const check = spec.slice(0, idx);
  if (!gateByCheck.has(check)) gateByCheck.set(check, []);
  gateByCheck.get(check).push(new RegExp(spec.slice(idx + 1)));
}
if (positional.length !== 2) {
  console.error('usage: baseline-diff.mjs <committed.json> <candidate.json> [--gate spec ...] [--allow-emptied check ...] [--examples N]');
  process.exit(2);
}
const [committedPath, candidatePath] = positional;

function load(p) {
  try {
    return JSON.parse(fs.readFileSync(p, 'utf8'));
  } catch (e) {
    console.error(`cannot read ${p}: ${e.message}`);
    process.exit(1);
  }
}
const committed = load(committedPath);
const candidate = load(candidatePath);

// Which rule a fingerprint belongs to. The key SHAPE differs per check and is
// load-bearing (see baseline.mjs occurrenceKey): doc_lint is
// `rule:file:#N:message` (rule at the HEAD), the two Vale checks are
// `file:#N:Check.Name` (check name at the TAIL — the merge gate's
// `Corosio\.PartHeadings$` is anchored on it), mrdocs_warnings is
// `file:#N:message`. Group accordingly rather than
// guessing from the string.
const afterOccurrenceIndex = (fp) => {
  const m = fp.match(/:#\d+:/);
  return m ? fp.slice(m.index + m[0].length) : fp;
};
function ruleOf(check, fp) {
  switch (check) {
    case 'vale_adoc':
    case 'vale_docstrings':
      return fp.slice(fp.lastIndexOf(':') + 1) || '(unparsed)';
    case 'doc_lint':
    // sentence_length shares doc_lint's fingerprint shape (rule at the HEAD), and
    // its three keys are the split a maintainer needs to see BEFORE reseeding:
    // `C2` is the hard slice that a `--gate 'sentence_length:^C2:'` spec will make
    // merge-blocking, `advisory-C2` is the design essays that never block, and
    // `BACKTICK` is a tooling diagnostic, not a prose finding. Without this case
    // all three collapsed into `(all)` and the reseed report hid exactly the
    // distinction that decides what becomes gate-protected.
    case 'sentence_length': {
      const i = fp.indexOf(':');
      return i > 0 ? fp.slice(0, i) : '(unparsed)';
    }
    case 'mrdocs_warnings':
      // Collapse quoted identifiers so per-symbol findings group by warning class.
      return afterOccurrenceIndex(fp).replace(/'[^']*'/g, "'…'").replace(/"[^"]*"/g, '"…"');
    default:
      return '(all)';
  }
}

const out = [];
const say = (s = '') => out.push(s);
const annotations = [];
const emptiedGated = [];
let fatal = false;

say('=== doc-lint baseline candidate: what committing it would change ===');
say(`committed: ${committedPath}  (generatedAt ${committed.generatedAt ?? '?'})`);
say(`candidate: ${candidatePath}  (generatedAt ${candidate.generatedAt ?? '?'})`);
say();

const checkNames = [...new Set([...Object.keys(committed.checks || {}), ...Object.keys(candidate.checks || {})])].sort();
const rows = [];
const perCheck = new Map();
for (const check of checkNames) {
  const base = committed.checks?.[check] ?? {};
  const cand = candidate.checks?.[check] ?? {};
  const baseSet = new Set(base.fingerprints || []);
  const candSet = new Set(cand.fingerprints || []);
  const added = [...candSet].filter((fp) => !baseSet.has(fp)).sort();
  const removed = [...baseSet].filter((fp) => !candSet.has(fp)).sort();
  const gateRes = gateByCheck.get(check) || null;
  const gatedAdded = gateRes ? added.filter((fp) => gateRes.some((re) => re.test(fp))) : [];
  perCheck.set(check, { base, cand, added, removed, gateRes, gatedAdded });
  rows.push([
    check + (gateRes ? ' *' : ''),
    cand.skipped ? 'SKIPPED' : String(cand.count ?? candSet.size),
    base.skipped ? 'SKIPPED' : String(base.count ?? baseSet.size),
    String(added.length),
    String(removed.length),
  ]);
  if (cand.skipped) {
    fatal = true;
    annotations.push(`::error title=Baseline candidate unusable::check '${check}' is SKIPPED in the candidate (${cand.reason ?? 'no reason given'}). Committing it would wipe that check's grandfathered backlog. Fix the environment and re-run.`);
  } else if (gateRes && candSet.size === 0 && baseSet.size > 0 && !allowEmptied.has(check)) {
    // A GATED check reporting zero findings where the committed baseline has some
    // is treated as a crash until proven otherwise. This is the fail-open that the
    // `skipped` flag does NOT catch: a check that dies with empty stdout used to be
    // recorded as `count: 0, skipped: false`, and the resulting candidate read
    // "retires 214, grandfathers 0, none gated" — an actively reassuring report for
    // a candidate that would wipe a merge-blocking check's entire backlog.
    // (baseline.mjs now marks such crashes skipped; this is the independent
    // second line, because it does not care WHY the slice is empty.)
    //
    // Emptiness, not a removal-fraction threshold: a crash produces exactly zero,
    // never 40% fewer, so emptiness targets the real failure mode with no magic
    // number and no arbitrary cliff. A percentage would fire on the very first
    // legitimate reseed here (mrdocs_warnings drops 195 of 214 = 91%), training
    // maintainers to wave it through — the worst outcome for a guard. The one
    // legitimate zero, a gated backlog genuinely closing, is a milestone worth an
    // explicit --allow-emptied <check>, which records the decision in the run log.
    fatal = true;
    emptiedGated.push(check);
    annotations.push(`::error title=Baseline candidate unusable::gated check '${check}' reports 0 findings but the committed baseline has ${baseSet.size}. A crashed check looks exactly like this. Verify the check really ran; if the backlog is genuinely closed, re-run with --allow-emptied ${check}.`);
  }
}

// Fixed-width table so before/after is skimmable in a job log.
const header = ['check', 'candidate', 'committed', 'added', 'removed'];
const widths = header.map((h, i) => Math.max(h.length, ...rows.map((r) => r[i].length)));
const fmt = (cells) => cells.map((c, i) => (i === 0 ? c.padEnd(widths[i]) : c.padStart(widths[i]))).join('  ');
say('--- per-check counts (`*` = gated by the merge gate) ---');
say(fmt(header));
say(widths.map((w) => '-'.repeat(w)).join('  '));
for (const r of rows) say(fmt(r));
const totalAdded = [...perCheck.values()].reduce((n, v) => n + v.added.length, 0);
const totalRemoved = [...perCheck.values()].reduce((n, v) => n + v.removed.length, 0);
const totalGated = [...perCheck.values()].reduce((n, v) => n + v.gatedAdded.length, 0);
say();
say(`TOTAL added ${totalAdded}   removed ${totalRemoved}   added-and-gated ${totalGated}`);
say();

function byRule(check, list) {
  const groups = new Map();
  for (const fp of list) {
    const rule = ruleOf(check, fp);
    if (!groups.has(rule)) groups.set(rule, []);
    groups.get(rule).push(fp);
  }
  return [...groups.entries()].sort((a, b) => b[1].length - a[1].length || a[0].localeCompare(b[0]));
}

// ADDED is the dangerous direction: every one of these becomes grandfathered.
say('--- ADDED fingerprints by check and rule (these become grandfathered) ---');
if (totalAdded === 0) say('(none)');
for (const check of checkNames) {
  const { added } = perCheck.get(check);
  if (added.length === 0) continue;
  say(`${check}: ${added.length} added`);
  for (const [rule, list] of byRule(check, added)) {
    say(`  ${String(list.length).padStart(5)}  ${rule}`);
    for (const fp of list.slice(0, examples)) say(`           e.g. ${fp}`);
    if (list.length > examples) say(`           ... and ${list.length - examples} more`);
  }
}
say();

// REMOVED is the point of a reseed: retiring entries that are already fixed.
say('--- REMOVED fingerprints by check and rule (backlog being retired) ---');
if (totalRemoved === 0) say('(none)');
for (const check of checkNames) {
  const { removed } = perCheck.get(check);
  if (removed.length === 0) continue;
  say(`${check}: ${removed.length} removed`);
  for (const [rule, list] of byRule(check, removed)) say(`  ${String(list.length).padStart(5)}  ${rule}`);
}
say();

say('--- ADDED fingerprints that the merge gate WOULD have blocked ---');
if (gateByCheck.size === 0) {
  say('(no --gate spec passed; re-run with the gate spec from .github/workflows/docs.yml to see this)');
} else if (totalGated === 0) {
  say(`(none — no added fingerprint matches the gate spec: ${[...gateByCheck.entries()].map(([c, res]) => res.map((re) => `${c}:${re.source}`).join(' ')).join(' ')})`);
} else {
  say(`${totalGated} added fingerprint(s) match the gate spec. Each is EITHER a real regression`);
  say('you are about to grandfather away, OR an environment difference. Account for every one');
  say('before committing this candidate:');
  for (const check of checkNames) {
    for (const fp of perCheck.get(check).gatedAdded) say(`  - ${check} :: ${fp}`);
  }
  annotations.push(`::error title=Baseline candidate adds gated findings::${totalGated} added fingerprint(s) would have blocked the merge gate. Do not commit this candidate until each is explained.`);
}
say();

if (fatal) {
  say('RESULT: candidate is NOT safe to commit. Do not use this file.');
  for (const check of checkNames) {
    if (candidate.checks?.[check]?.skipped) say(`  - ${check} is SKIPPED in the candidate: the check could not run at all.`);
  }
  for (const check of emptiedGated) {
    say(`  - ${check} is GATED and reports 0 findings against ${committed.checks?.[check]?.count ?? '?'} in the`);
    say('    committed baseline. A crashed check looks exactly like this. Confirm the check really');
    say(`    ran; if that backlog is genuinely closed, re-run with --allow-emptied ${check}.`);
  }
} else if (totalGated > 0) {
  say('RESULT: candidate must not be committed until its gated additions are justified (see above).');
} else {
  say(`RESULT: candidate retires ${totalRemoved} and grandfathers ${totalAdded} fingerprint(s), none gated.`);
  if (allowEmptied.size > 0) say(`(--allow-emptied accepted a zero-finding gated check: ${[...allowEmptied].join(', ')})`);
}

console.log(out.join('\n'));
if (process.env.GITHUB_ACTIONS) for (const a of annotations) console.log(a);
// Gated additions exit 1 too: the skip path already fails closed, and a green step
// beside a red annotation is how a warning gets skimmed past. An ungated addition
// alone is not an error (those slices are still being worked down).
process.exit(fatal || totalGated > 0 ? 1 : 0);
