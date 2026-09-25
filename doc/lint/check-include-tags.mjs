#!/usr/bin/env node
//
// check-include-tags.mjs — every `include::example$...[tag=...]` in a page
// must resolve to a real file that really carries that tag. Node built-ins
// only, no dependencies.
//
// Why this exists: pages pull code out of compiled snippets and, since
// issue #381, straight out of the public headers, so a definition shown on
// a page cannot drift from the definition that ships. That moves the risk
// rather than removing it. Asciidoctor treats a missing include tag as a
// WARNING and still exits 0 — verified locally: a page referencing a
// nonexistent tag renders an empty listing block and the build succeeds.
// The Antora CI leg cannot catch it either; it only asserts that
// `build/site` exists, precisely because Antora also exits 0 on failure.
//
// So without this gate, deleting a `tag::`/`end::` marker from a header
// during ordinary refactoring silently empties whatever page included it,
// and nothing goes red. That is a worse failure than the drift it replaced:
// drift is at least visible on the page.
//
// The example$ -> repo-path mapping is read out of doc/antora.yml's
// collector scan config rather than hardcoded here, so adding a scan entry
// cannot leave this check behind.
//
// Blocking: exits 1 on any unresolved include target or missing tag.
//
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DOC_DIR = path.resolve(SCRIPT_DIR, '..');
const REPO_ROOT = path.resolve(DOC_DIR, '..');
const PAGES_DIR = path.join(DOC_DIR, 'modules', 'ROOT', 'pages');
const EXAMPLES_PREFIX = 'modules/ROOT/examples';

// Parse the `dir:`/`into:` pairs under ext.collector.scan in antora.yml.
// A full YAML parser would be a dependency; the block is a flat list of
// two- and three-key entries, so an indentation-aware line scan is enough.
function readScanMap(yamlPath) {
  const lines = fs.readFileSync(yamlPath, 'utf8').split('\n');
  const entries = [];
  let cur = null;
  for (const line of lines) {
    if (/^\s*#/.test(line)) continue;
    const dir = line.match(/^\s*-\s*dir:\s*(\S+)\s*$/);
    if (dir) {
      if (cur) entries.push(cur);
      cur = { dir: dir[1], into: null };
      continue;
    }
    const into = line.match(/^\s*into:\s*(\S+)\s*$/);
    if (into && cur) {
      cur.into = into[1];
      entries.push(cur);
      cur = null;
    }
  }
  if (cur) entries.push(cur);

  // example$<sub>/<rest> -> <dir>/<rest>. Longest prefix wins, so a nested
  // mapping such as examples/snippets is preferred over bare examples.
  return entries
    .filter((e) => e.into && e.into.startsWith(EXAMPLES_PREFIX))
    .map((e) => ({
      prefix: e.into.slice(EXAMPLES_PREFIX.length).replace(/^\//, ''),
      dir: e.dir,
    }))
    .sort((a, b) => b.prefix.length - a.prefix.length);
}

function resolveTarget(resource, scanMap) {
  for (const { prefix, dir } of scanMap) {
    if (prefix === '') return path.join(REPO_ROOT, dir, resource);
    if (resource === prefix || resource.startsWith(prefix + '/')) {
      return path.join(REPO_ROOT, dir, resource.slice(prefix.length).replace(/^\//, ''));
    }
  }
  return null;
}

// tag=a  |  tags=a;b;!c  |  tags=a,b — negations and wildcards select
// nothing on their own, so they are not names this check can verify.
function tagsOf(attrs) {
  const m = attrs.match(/\btags?=([^,\]]*)/);
  if (!m) return [];
  return m[1]
    .split(/[;,]/)
    .map((t) => t.trim())
    .filter((t) => t && !t.startsWith('!') && !t.includes('*'));
}

function walk(dir, out = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (e.name.endsWith('.adoc')) out.push(p);
  }
  return out;
}

const scanMap = readScanMap(path.join(DOC_DIR, 'antora.yml'));
if (scanMap.length === 0) {
  console.error('check-include-tags: no collector scan entries found in antora.yml');
  process.exit(1);
}

const violations = [];
let checked = 0;

for (const page of walk(PAGES_DIR)) {
  const rel = path.relative(REPO_ROOT, page);
  const lines = fs.readFileSync(page, 'utf8').split('\n');
  lines.forEach((line, i) => {
    const m = line.match(/^include::example\$(\S+?)\[([^\]]*)\]/);
    if (!m) return;
    const [, resource, attrs] = m;
    const where = `${rel}:${i + 1}`;
    const target = resolveTarget(resource, scanMap);
    if (!target) {
      violations.push(`${where}: example$${resource} matches no collector scan entry`);
      return;
    }
    if (!fs.existsSync(target)) {
      violations.push(`${where}: example$${resource} resolves to a missing file (${path.relative(REPO_ROOT, target)})`);
      return;
    }
    const body = fs.readFileSync(target, 'utf8');
    for (const tag of tagsOf(attrs)) {
      checked++;
      const has = body.includes(`tag::${tag}[]`) && body.includes(`end::${tag}[]`);
      if (!has) {
        violations.push(`${where}: tag '${tag}' not found in ${path.relative(REPO_ROOT, target)}`);
      }
    }
  });
}

if (violations.length) {
  console.error(`check-include-tags: ${violations.length} violation(s)\n`);
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}

console.log(`check-include-tags: OK — ${checked} tagged include(s) resolve to a live tag.`);
