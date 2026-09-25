#!/usr/bin/env node
//
// mrdocs-warnings.mjs — reference-surface gate (Style Guide Part F.0,
// "MrDocs-no-warnings"). Node built-ins only, no dependencies.
//
// MrDocs has no standalone CLI package: it runs inside
// @cppalliance/antora-cpp-reference-extension during `npx antora`. Scanning
// the *captured Antora build log* does not work — the extension's runCommand
// helper only forwards MrDocs's stderr to the console; MrDocs prints its
// per-symbol "undocumented"/"missing param doc" findings to stdout, and the
// extension swallows stdout into an internal buffer it never surfaces
// (lib/extension.js, runCommand: `output` is not set for the MrDocs
// invocation, so `ps.stdout` data goes to an array, not the console).
// Verified locally: an Antora build with mrdocs.yml's warning flags on
// produced 0 visible findings in the build log, while invoking the same
// MrDocs binary/config/args directly produced 208.
//
// So this script invokes MrDocs directly, with the same config file and CLI
// arguments the extension uses (mirrored from its debug log), then parses
// combined stdout+stderr itself. It mirrors the extension's own compiler
// preference (clang++/clang over g++/gcc — the extension does this because
// this MrDocs build crashes with a stray GCC-only header path; reproduced
// locally with GCC 16.1.1) so results match what a real Antora build would
// hit if not for the stdout-swallowing bug above.
//
// Non-blocking (Task 2): always exits 0. Findings are JSON on stdout.
//
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DOC_DIR = path.resolve(SCRIPT_DIR, '..');
const REPO_ROOT = path.resolve(DOC_DIR, '..');
const CONFIG_PATH = path.join(DOC_DIR, 'mrdocs.yml');

function emit(payload) {
  console.log(JSON.stringify(payload, null, 2));
  process.exit(0);
}

// MRDOCS_ROOT, when set, is AUTHORITATIVE and no version check is applied to it.
// doc/build_antora.sh installs the reference-snippets extension into exactly one
// MrDocs and exports MRDOCS_ROOT (writing it to $GITHUB_ENV so it survives into
// later workflow steps). That install is the one the rendered reference was
// generated with, which is the invariant this check actually needs: measure the
// reference surface with the same MrDocs that produced it.
//
// It must not be version-matched, because the `develop-release` asset is a moving
// target. Measured: the same asset name reported `0.8.0+e31308f6c944` locally and
// `2026.9.5` in CI days apart. A pin against it fails CLOSED but uselessly — the
// check reports SKIPPED, and a reseed then tries to wipe the whole grandfathered
// mrdocs_warnings backlog. That is exactly what happened on the first CI reseed,
// and the baseline-diff safety net is what caught it.
//
// The pin survives only as a tiebreaker for the FALLBACK cache scan, where the
// reference-collector cache can hold several `mrdocs` binaries (the `develop` and
// `master` tags) and picking the first would be nondeterministic. Override with
// MRDOCS_VERSION. The resolved binary and its reported version are emitted in the
// payload either way, so a scheme change is visible instead of silent.
const PINNED_VERSION = process.env.MRDOCS_VERSION || '0.8.0';

function findOnPath(names) {
  return findAllOnPath(names)[0] || null;
}

function findAllOnPath(names) {
  const found = [];
  const dirs = (process.env.PATH || '').split(path.delimiter);
  for (const name of names) {
    for (const dir of dirs) {
      const candidate = path.join(dir, name);
      try {
        fs.accessSync(candidate, fs.constants.X_OK);
        found.push(candidate);
      } catch { /* not here */ }
    }
  }
  return found;
}

// Search the Antora reference-collector cache the extension populates
// (getUserCacheDir('antora')/reference-collector/mrdocs/<platform>/<tag>/bin/mrdocs).
// Returns EVERY executable found so the caller can pick the pin-matching one.
function findMrDocsUnder(...bases) {
  const found = [];
  bases = bases.filter(Boolean);
  for (const base of bases) {
    if (!fs.existsSync(base)) continue;
    const stack = [base];
    while (stack.length) {
      const dir = stack.pop();
      let entries;
      try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { continue; }
      for (const ent of entries) {
        const p = path.join(dir, ent.name);
        if (ent.isDirectory()) stack.push(p);
        else if (ent.name === 'mrdocs' || ent.name === 'mrdocs.exe') {
          try { fs.accessSync(p, fs.constants.X_OK); found.push(p); } catch { /* skip */ }
        }
      }
    }
  }
  return found;
}

// Base version (X.Y.Z, build metadata after `+` stripped) reported by a
// candidate binary, or null if it can't be run / parsed.
function mrdocsBaseVersion(exe) {
  const r = spawnSync(exe, ['--version'], { encoding: 'utf8' });
  if (r.error || (r.status !== 0 && r.status !== null)) return null;
  const out = `${r.stdout || ''}${r.stderr || ''}`;
  const m = out.match(/MrDocs\s+version\s+(\S+)/i) || out.match(/(\d+\.\d+\.\d+)/);
  return m ? m[1].split('+')[0] : null;
}

// MRDOCS_ROOT wins outright when it is set; otherwise fall back to PATH and the
// reference-collector cache, where the pin disambiguates.
const rootCandidates = findMrDocsUnder(process.env.MRDOCS_ROOT);
const candidates = rootCandidates.length
  ? rootCandidates
  : [...findAllOnPath(['mrdocs', 'mrdocs.exe']),
     ...findMrDocsUnder(path.join(os.homedir(), '.cache/antora/reference-collector/mrdocs'))];
if (candidates.length === 0) {
  emit({
    error: 'mrdocs executable not found (checked PATH and the Antora reference-collector cache). ' +
           'Run the Antora build first (it downloads MrDocs), then re-run this script.',
    summary: { total: 0 },
    findings: [],
  });
}

// From MRDOCS_ROOT: take it as-is. From the fallback scan: take the first whose
// BASE version (the `X.Y.Z` before any `+build` metadata) matches the pin.
let mrdocsExe = null;
let mrdocsVersion = null;
const inspected = [];
if (rootCandidates.length) {
  mrdocsExe = rootCandidates[0];
  mrdocsVersion = mrdocsBaseVersion(mrdocsExe);
  inspected.push(`${mrdocsExe} => ${mrdocsVersion ?? '(version unreadable)'} [MRDOCS_ROOT]`);
  if (mrdocsVersion === null) {
    // Unreadable means it cannot be run at all. Fail rather than scan with it:
    // a skipped check is what wipes a gated backlog on the next reseed.
    emit({
      error: `MRDOCS_ROOT names a MrDocs that will not report a version: ${mrdocsExe}`,
      summary: { total: 0 },
      findings: [],
    });
  }
} else {
  for (const c of candidates) {
    const v = mrdocsBaseVersion(c);
    inspected.push(`${c} => ${v ?? '(version unreadable)'}`);
    if (v === PINNED_VERSION) { mrdocsExe = c; mrdocsVersion = v; break; }
  }
  if (!mrdocsExe) {
    emit({
      error: `no MrDocs binary matching pinned version ${PINNED_VERSION} found, and ` +
             `MRDOCS_ROOT is not set (set MRDOCS_ROOT to the install the docs were ` +
             `built with, or MRDOCS_VERSION to change the fallback pin). ` +
             `Candidates inspected:\n  ${inspected.join('\n  ')}`,
      summary: { total: 0 },
      findings: [],
    });
  }
}

if (!fs.existsSync(CONFIG_PATH)) {
  emit({ error: `mrdocs.yml not found: ${CONFIG_PATH}`, summary: { total: 0 }, findings: [] });
}

// Mirror CppReferenceExtension.findCXXCompilers(): clang++/clang preferred over g++/gcc.
const cxx = findOnPath(['clang++']) || process.env.CXX_COMPILER || process.env.CXX || findOnPath(['g++']);
const cc = findOnPath(['clang']) || process.env.C_COMPILER || process.env.CC || findOnPath(['gcc']);

const outDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mrdocs-warnings-'));
const args = [
  `--config=${CONFIG_PATH}`,
  `--output=${outDir}`,
  '--generator=adoc',
  '--multipage=true',
  '--tagfile=reference.tag.xml',
];

const result = spawnSync(mrdocsExe, args, {
  cwd: REPO_ROOT,
  env: { ...process.env, ...(cxx ? { CXX: cxx, CMAKE_CXX_COMPILER: cxx } : {}), ...(cc ? { CC: cc, CMAKE_C_COMPILER: cc } : {}) },
  encoding: 'utf8',
  maxBuffer: 64 * 1024 * 1024,
});

fs.rmSync(outDir, { recursive: true, force: true });

if (result.error || (result.status !== 0 && result.status !== null)) {
  emit({
    error: `mrdocs exited with status ${result.status}: ${result.error?.message || '(see stderr)'}`,
    stderrTail: (result.stderr || '').slice(-2000),
    summary: { total: 0 },
    findings: [],
  });
}

const stripAnsi = (s) => s.replace(/\x1b\[[0-9;]*m/g, '');
const relIncludePath = (p) => p.replace(/^.*?(include\/boost\/corosio\/.*)$/, '$1');
const combined = stripAnsi(`${result.stdout || ''}\n${result.stderr || ''}`);
const lines = combined.split('\n');

// MrDocs prints "<path>:<line>:<col>:" then indented "N) <message>" items for
// that location; separately it prints bare "warning: ..." lines (e.g.
// unsupported HTML tags) with no location. CMake's own "CMake Warning" noise
// is excluded — it is a build-system warning, not a reference-surface one.
const LOC_RE = /^(\/\S+\.(?:hpp|cpp|ipp)):(\d+):(\d+):\s*$/;
const ITEM_RE = /^\s*\d+\)\s*(.+)$/;
const BARE_WARNING_RE = /^warning:\s*(.+)$/;

// `boost`/`corosio` namespaces get flagged by warn-if-undocumented too, but a
// namespace isn't a documentable symbol in the sense this gate cares about
// (no @brief slot maps onto a namespace declaration the way it does onto a
// class/function), and the finding is fingerprint-unstable across
// environments (see the file header: local vs. CI MrDocs build/order
// differences). Drop these here so they never enter the gate's finding list,
// in both baseline generation and the live check (same script). All other
// MrDocs warning classes (undocumented symbol/param, broken refs, etc.) are
// left intact.
const NAMESPACE_UNDOCUMENTED_RE = /namespace is undocumented/;

const findings = [];
let currentLoc = null;
for (const line of lines) {
  const loc = line.match(LOC_RE);
  if (loc) {
    currentLoc = { file: relIncludePath(loc[1]), line: Number(loc[2]) };
    continue;
  }
  const item = line.match(ITEM_RE);
  if (item && currentLoc) {
    const message = item[1].trim();
    if (!NAMESPACE_UNDOCUMENTED_RE.test(message)) {
      findings.push({ file: currentLoc.file, line: currentLoc.line, message });
    }
    continue;
  }
  const bare = line.match(BARE_WARNING_RE);
  if (bare) {
    const message = bare[1].trim();
    if (!NAMESPACE_UNDOCUMENTED_RE.test(message)) {
      findings.push({ file: null, line: null, message });
    }
  }
}

// Report which binary was used and what version it claims, so a develop-release
// version-scheme change is visible in the run log instead of silent.
emit({ mrdocs: { exe: mrdocsExe, version: mrdocsVersion, source: rootCandidates.length ? 'MRDOCS_ROOT' : 'pin' },
       summary: { total: findings.length }, findings });
