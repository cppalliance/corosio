#!/usr/bin/env node
//
// check-no-new-violations.mjs — the "no NEW violations" gate the Task 2
// brief's acceptance criterion describes: a fresh check run is diffed
// against doc/lint/baseline.json (Style Guide Part F.0) per-check
// fingerprint sets, and anything not already in the baseline is reported as
// new. This is what would catch a newly introduced banned word ("utilize")
// or a newly undocumented @param, without re-flagging the existing backlog.
//
// Node built-ins only. Regenerates a fresh snapshot via baseline.mjs into a
// temp file (never overwrites the committed baseline.json) and compares.
//
// Non-blocking by default (Task 2: everything stays warning-mode). Pass
// --strict to exit 1 when new violations are found.
//
// --gate <check>:<rule-regex>  restricts what counts as blocking to a named
// set of (check, rule-regex) pairs — the phase-exit gate-promotion mechanism.
// Repeatable. Each value is split on its FIRST ':' into a check name and a
// regex tested against that check's fingerprints. In gate mode BOTH the exit
// condition and the skip check are filtered: only NEW fingerprints of a gated
// check that match its regex block, and only a skip of a GATED check fails the
// run ("can't verify a gated rule = not a pass"); skips of non-gated checks
// (mrdocs, vale_docstrings) are reported but do not block. Without
// --gate the comparator stays omnibus (the non-blocking report step).
//
// A gated check that reports ZERO findings against a non-empty baseline also
// fails the gate — a check that silently did not run is indistinguishable from
// one that ran clean, and `skipped` does not catch it. See the rule at the
// bottom of the per-check loop for the reachable case and the reasoning.
//
// Phase-1 exit gate spec (A1/A6/A7/B2/D2):
//   --gate 'doc_lint:^(A1|A6|B2|D2):' --gate 'vale_adoc:Corosio\.PartHeadings$'
//   (A1/A6/B2/D2 come from doc_lint; A7 is the Vale rule Corosio.PartHeadings.)
//
// Phase-2 exit adds MrDocs-no-warnings to the above (full spec):
//   --gate 'doc_lint:^(A1|A6|B2|D2):' --gate 'vale_adoc:Corosio\.PartHeadings$' \
//   --gate 'mrdocs_warnings:.*'
//   mrdocs_warnings:.* gates the whole reference-surface check. E4 (accessibility
//   contrast) is NOT gated and has no check at all here: it is Review tier
//   (doc/STYLE_GUIDE.md Part F.0), and Corosio deliberately does not port Capy's
//   pa11y scan, because a check that cannot fail CI earns nothing. See
//   doc/design/style-guide-compliance.md section 4.4.
//
// --allow-emptied <check>  suppresses the "gated check reports zero findings
// against a non-empty baseline" failure described below, for one check. Use it
// only when a gated backlog has genuinely closed; it records the decision in
// the run log. Not used by the committed CI invocation.
//
// Usage: node doc/lint/check-no-new-violations.mjs [--strict] [--gate spec ...]
//        [--allow-emptied check ...]
//
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const strict = argv.includes('--strict');
// --show-baseline additionally lists findings that are present RIGHT NOW and are
// grandfathered by baseline.json, as warnings. That is deliberately the live
// intersection (current ∩ baseline), not the baseline file's contents: the
// baseline is only ever reseeded when something is ADDED, so it accumulates dead
// clauses for findings that were long since fixed. Printing the file would list
// thousands of already-fixed items; printing the intersection is the real
// remaining worklist.
const showBaseline = argv.includes('--show-baseline');
// --json restores the machine-readable dump for tooling. Nothing in-tree parses
// it today; the CI steps read the human output.
const jsonOut = argv.includes('--json');

// Parse --gate specs; everything else (except --strict) is passed through to
// baseline.mjs. NB: --gate values must NOT reach baseline.mjs, whose first
// non-flag arg is taken as the output path.
const gateByCheck = new Map(); // check -> [RegExp]
const allowEmptied = new Set(); // checks whose zero is an accepted milestone
const extraArgs = [];
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--strict' || a === '--show-baseline' || a === '--json') continue;
  let allow = null;
  if (a === '--allow-emptied') allow = argv[++i];
  else if (a.startsWith('--allow-emptied=')) allow = a.slice('--allow-emptied='.length);
  if (allow != null) {
    if (!allow) {
      console.error('--allow-emptied expects a check name');
      process.exit(2);
    }
    allowEmptied.add(allow);
    continue;
  }
  let spec = null;
  if (a === '--gate') spec = argv[++i];
  else if (a.startsWith('--gate=')) spec = a.slice('--gate='.length);
  if (spec != null) {
    const idx = spec.indexOf(':');
    if (idx < 0) {
      console.error(`--gate expects <check>:<rule-regex>, got: ${spec}`);
      process.exit(2);
    }
    const check = spec.slice(0, idx);
    const re = new RegExp(spec.slice(idx + 1));
    if (!gateByCheck.has(check)) gateByCheck.set(check, []);
    gateByCheck.get(check).push(re);
    continue;
  }
  extraArgs.push(a);
}
const gated = gateByCheck.size > 0;

const baselinePath = path.join(SCRIPT_DIR, 'baseline.json');
if (!fs.existsSync(baselinePath)) {
  console.log(JSON.stringify({ error: `no baseline.json at ${baselinePath} — run baseline.mjs first` }, null, 2));
  process.exit(0);
}
const baseline = JSON.parse(fs.readFileSync(baselinePath, 'utf8'));

const tmpPath = path.join(os.tmpdir(), `doc-lint-current-${process.pid}.json`);
const r = spawnSync('node', [path.join(SCRIPT_DIR, 'baseline.mjs'), '--details', ...extraArgs, tmpPath], { encoding: 'utf8' });
if (r.status !== 0 || !fs.existsSync(tmpPath)) {
  console.log(JSON.stringify({ error: 'failed to generate a current snapshot', stderr: r.stderr }, null, 2));
  process.exit(0);
}
const current = JSON.parse(fs.readFileSync(tmpPath, 'utf8'));
fs.rmSync(tmpPath, { force: true });

let totalNew = 0;        // omnibus: new findings across ALL checks (report semantics)
let anySkipped = false;  // any check skipped at all
let gatedNew = 0;        // new findings in gated checks matching a gate regex
let gatedSkipped = false; // a GATED check was skipped (can't verify => gate fails)
let gatedEmptied = false; // a GATED check reported ZERO findings against a non-empty baseline
const gatedFindings = []; // the specific gated new fingerprints (named in the log)
const emptiedGated = [];  // the checks that tripped the emptiness rule
const report = {};
for (const [check, currentCheck] of Object.entries(current.checks)) {
  const gateRes = gateByCheck.get(check) || null;
  // A skipped check (Vale broken, MrDocs couldn't run, ...) is NOT a clean pass — it
  // means no comparison happened at all. Surface it loudly (stderr, outside the JSON blob)
  // so it can't be mistaken for "0 new" in a log that only skims the summary line, and
  // record it distinctly (newCount: null, not 0) in the JSON report too. A skip of a GATED
  // check additionally fails the gate: an unverifiable gated rule is not a pass.
  if (currentCheck.skipped) {
    anySkipped = true;
    if (gateRes) gatedSkipped = true;
    console.error(`SKIPPED: ${check} (${currentCheck.reason}) — no-new-violations comparison NOT performed for this check.${gateRes ? ' [GATED — fails the gate]' : ''}`);
    report[check] = {
      skipped: true, reason: currentCheck.reason, gated: !!gateRes,
      baselineCount: baseline.checks[check]?.count ?? 0, currentCount: currentCheck.count,
      newCount: null, newFindings: [],
    };
    continue;
  }
  const baseSet = new Set(baseline.checks[check]?.fingerprints || []);
  const currentSet = currentCheck.fingerprints || [];
  const newOnes = currentSet.filter((fp) => !baseSet.has(fp));
  const stillPresent = currentSet.filter((fp) => baseSet.has(fp));
  totalNew += newOnes.length;
  const entry = { baselineCount: baseline.checks[check]?.count ?? 0, currentCount: currentCheck.count, newCount: newOnes.length, newFindings: newOnes };
  entry.stillPresent = stillPresent;
  entry.details = currentCheck.details || {};
  if (gateRes) {
    const gatedOnes = newOnes.filter((fp) => gateRes.some((re) => re.test(fp)));
    entry.gated = true;
    entry.gatedNewCount = gatedOnes.length;
    entry.gatedNewFindings = gatedOnes;
    gatedNew += gatedOnes.length;
    for (const fp of gatedOnes) gatedFindings.push(`${check} :: ${fp}`);

    // A GATED check that reports ZERO findings where the committed baseline has
    // some is treated as a check that did not run, until proven otherwise. This
    // is the fail-open the `skipped` flag does NOT catch, and it is reachable:
    //
    //   $ cd doc && vale --output=JSON lint/.nonexistent-corpus
    //   {}
    //   $ echo $?
    //   0
    //
    // baseline.mjs marks a Vale check skipped only on exit 2 or a non-object
    // parse, so exit 0 plus `{}` yields `{count: 0, skipped: false}` — and this
    // comparator then computes "zero new" from an empty current set and reports
    // `gated: true, gatedNew: 0`, i.e. a gate that says it is gating while
    // measuring nothing. Any renamed corpus path, crashed extractor, or
    // `.vale.ini` edit that stops matching the corpus lands here.
    //
    // The rule is the same one baseline-diff.mjs applies to a reseed candidate,
    // for the same reasons: emptiness rather than a removal-fraction threshold
    // (a check that did not run produces exactly zero, never 40% fewer), and
    // scoped to GATED checks, whose zero is the one that decides a merge.
    //
    // Deliberately WHOLE-CHECK, not per-gate-regex. The gated SLICE of
    // vale_docstrings is legitimately empty today — zero Corosio.SimpleTense /
    // NoFluff / Terminology on the docstring corpus is exactly what Phase 4
    // delivered — so a per-slice rule would fail the committed invocation on
    // the phase's own success state. A whole-check zero cannot be produced by
    // wording work: the residual Vale.Spelling/Google backlog on both corpora
    // is not going to zero, so only a broken run gets there.
    //
    // The one legitimate whole-check zero, a gated backlog genuinely closing,
    // is a milestone worth an explicit --allow-emptied <check>.
    if (currentSet.length === 0 && baseSet.size > 0 && !allowEmptied.has(check)) {
      entry.gatedEmptied = true;
      gatedEmptied = true;
      emptiedGated.push(check);
    } else if (currentSet.length === 0 && baseSet.size > 0) {
      entry.gatedEmptiedAllowed = true;
    }
  }
  report[check] = entry;
}

if (anySkipped) {
  console.error(`SKIPPED checks present — totalNew (${totalNew}) is only valid for the checks that actually ran.`);
}

// The blocking condition: gated slice when --gate is present, omnibus otherwise.
const blockingNew = gated ? gatedNew : totalNew;
const blockingSkip = gated ? gatedSkipped : anySkipped;

// ---------------------------------------------------------------------------
// Human-readable report.
//
// This used to print the whole comparison as JSON. On a real failure that was
// ~250 lines of which 8 mattered, and the 8 carried no line number and no
// quote — so the reader still had to grep the corpus to find out what broke.
// The rule now: say what is wrong, where, and show enough of it to recognise.
const isCI = !!process.env.GITHUB_ACTIONS;
const fmtLoc = (d, fp) => (d && d.file ? `${d.file}${d.line ? `:${d.line}` : ''}` : fp);

// One finding, three lines at most: location, rule + message, excerpt.
function emit(level, check, fp, d) {
  const loc = fmtLoc(d, fp);
  const rule = d?.rule ? d.rule : check;
  const msg = d?.message || fp;
  console.error(`${level}  ${loc}`);
  console.error(`       [${check} ${rule}] ${msg}`);
  if (d?.excerpt) console.error(`       "${d.excerpt}"`);
  // GitHub annotations put the finding on the diff line itself. Only for
  // findings that actually block: a non-blocking step annotating every new
  // finding buries the handful that matter under a hundred that do not.
  //
  // Emitted as ::warning, not ::error, deliberately. The annotation's job is
  // to locate the finding on the diff; the check status is what says the run
  // failed, and it already does (exit 1). A red inline marker on prose nits
  // reads as broken code to anyone skimming the Files-changed tab.
  if (isCI && level === 'ERROR' && d?.file) {
    const esc = (t) => String(t).replace(/%/g, '%25').replace(/\r/g, '%0D').replace(/\n/g, '%0A');
    console.log(`::warning file=${d.file}${d.line ? `,line=${d.line}` : ''}::${esc(`[${check} ${rule}] ${msg}`)}`);
  }
}

// Errors: the findings that actually block. Under --gate that is the gated
// slice; without --gate every new finding is reported as an error, because
// then there is no narrower thing to mean.
// Two independent questions, and conflating them has bitten twice:
//
//   * WHICH findings deserve attention — the gated slice, when --gate is given.
//     Those get called out and annotated on the diff.
//   * WHETHER the run fails — --strict, and nothing else.
//
// Keying attention on `gated` alone made the un-gated report step announce all
// ~124 new findings as blocking and annotate every one. Keying it on `strict`
// instead then meant that dropping --strict silently removed the annotations
// too. The gate spec says what matters; --strict only says whether mattering is
// fatal.
const errorList = [];
const newNonBlocking = [];
for (const [check, e] of Object.entries(report)) {
  if (e.skipped) continue;
  const gatedSet = new Set(e.gatedNewFindings || []);
  for (const fp of e.newFindings || []) {
    (gated && gatedSet.has(fp) ? errorList : newNonBlocking).push([check, fp, e.details?.[fp]]);
  }
}

if (errorList.length) {
  console.error(`\n${errorList.length} new gated violation(s) (${strict
    ? 'these block the merge'
    : 'reported, not blocking — fix them before this gate is promoted'}):\n`);
  for (const [check, fp, d] of errorList) emit('ERROR', check, fp, d);
}

if (newNonBlocking.length) {
  console.error(`\n${newNonBlocking.length} other new finding(s) since the baseline ` +
    `(${gated ? 'not in a gated slice' : 'report only'}):\n`);
  for (const [check, fp, d] of newNonBlocking) emit('NEW  ', check, fp, d);
}

for (const check of emptiedGated) {
  console.error(`\nERROR  gated check '${check}' reports 0 findings but the committed baseline has ` +
    `${baseline.checks[check]?.fingerprints?.length ?? 0} — a check that did not run looks exactly ` +
    `like this. Verify it really ran; if the backlog is genuinely closed, re-run with ` +
    `--allow-emptied ${check}.`);
}

if (showBaseline) {
  const live = [];
  for (const [check, e] of Object.entries(report)) {
    if (e.skipped) continue;
    for (const fp of e.stillPresent || []) live.push([check, fp, e.details?.[fp]]);
  }
  console.error(`\n${live.length} grandfathered finding(s) still present — the remaining backlog:\n`);
  for (const [check, fp, d] of live) emit('WARN ', check, fp, d);
}

if (!errorList.length && !newNonBlocking.length && !emptiedGated.length && !anySkipped) {
  console.error(`No new ${gated ? 'gated ' : ''}findings.` +
    (showBaseline ? '' : ' Re-run with --show-baseline to list the remaining backlog.'));
}

// ---------------------------------------------------------------------------
// GitHub job summary. Annotations land on the diff, but a fork PR cannot be
// commented on (the pull_request token is read-only), so the run page is the
// one place a full report is reachable from the check without extra machinery.
// Same pattern ci.yml and the reseed step already use.
if (process.env.GITHUB_STEP_SUMMARY) {
  const md = [];
  // Pipes and newlines would break the table; excerpts are prose and can hold both.
  const cell = (t) => String(t ?? '').replace(/\|/g, '\\|').replace(/\r?\n/g, ' ');

  // The blocking findings are deliberately NOT tabulated here. They are already
  // annotated on the diff, which is where you act on them; repeating them on the
  // run page just means reading the same eight things twice, in the place that is
  // one click further away.
  if (errorList.length) {
    md.push(`### ${strict ? '❌' : '⚠️'} ${errorList.length} new gated violation(s)` +
            ' — see the annotations on the diff', '');
  } else if (!emptiedGated.length && gated) {
    md.push('### ✅ No new gated violations', '');
  }

  for (const check of emptiedGated) {
    md.push(`### ❌ Gated check \`${check}\` reported zero findings`, '',
      `The committed baseline has ${baseline.checks[check]?.fingerprints?.length ?? 0}. ` +
      'A check that did not run looks exactly like this — verify it ran before believing it.', '');
  }

  if (newNonBlocking.length) {
    const why = gated ? 'not in a gated slice' : 'report only';
    md.push(`<details><summary>${newNonBlocking.length} new finding(s) since the baseline — ${why}</summary>`, '', '```');
    for (const [check, fp, d] of newNonBlocking.slice(0, 200)) md.push(`${fmtLoc(d, fp)}  [${d?.rule || check}] ${d?.message || ''}`);
    if (newNonBlocking.length > 200) md.push(`… and ${newNonBlocking.length - 200} more`);
    md.push('```', '</details>', '');
  }

  if (showBaseline) {
    const live = [];
    for (const [check, e] of Object.entries(report)) {
      if (e.skipped) continue;
      for (const fp of e.stillPresent || []) live.push([check, fp, e.details?.[fp]]);
    }
    md.push(`<details><summary>${live.length} grandfathered finding(s) still present — the remaining backlog</summary>`, '', '```');
    for (const [check, fp, d] of live) md.push(`${fmtLoc(d, fp)}  [${d?.rule || check}] ${d?.message || ''}`);
    md.push('```', '</details>', '');
  }

  for (const [check, e] of Object.entries(report)) {
    if (e.skipped) md.push(`> ⚠️ \`${check}\` was **skipped** (${cell(e.reason)}) — not compared.`, '');
  }

  fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, md.join('\n') + '\n');
}

if (jsonOut) {
  console.log(JSON.stringify({
    totalNew, anySkipped, strict,
    gated, gatedNew: gated ? gatedNew : undefined, gatedSkipped: gated ? gatedSkipped : undefined,
    gatedEmptied: gated ? gatedEmptied : undefined,
    emptiedGated: gated ? emptiedGated : undefined,
    gatedFindings: gated ? gatedFindings : undefined,
    checks: report,
  }, null, 2));
}
// process.exit() truncates buffered stdout when stdout is a pipe — it does not
// wait for the flush. That silently cut the --json payload mid-string. Setting
// exitCode lets Node drain normally and exit with the same status.
process.exitCode = strict && (blockingNew > 0 || blockingSkip || (gated && gatedEmptied)) ? 1 : 0;
