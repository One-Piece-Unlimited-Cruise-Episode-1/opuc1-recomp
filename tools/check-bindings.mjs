// Checks every HLE binding in the rebound runtime against the OPUC function map.
//   node tools/check-bindings.mjs [--list]
// For each registration site (PPC_NATIVE_OVERRIDE[_VOID] / REGISTER_NATIVE_*) that carries an OPUC address:
//   - the OPUC address must be a function start in projects/opuc1/MAP.txt,
//   - the OPUC name must normalize to the MKWii symbol the upstream site was bound to, unless the address comes
//     from projects/opuc1/rebind_overrides.tsv (then the override note is the evidence),
//   - no two registrations may bind the same OPUC address.
// An OPUC-native registration (no /*mkw:*/ marker) may also sit at an address listed in projects/opuc1/native_entries.tsv:
//   slot  = code-free address below the first MAP entry, thunk = 4-aligned address inside a MAP entry (a merged `b` thunk).
// Exit code 1 when any binding fails. Also prints the binding table with --list.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const rd = f => fs.readFileSync(path.join(root, f), 'utf8');
const norm = n => {
  n = n.replace(/^RVL::/, '');
  const m = n.match(/^(__)?([A-Za-z0-9]+)::(.*)$/); if (!m) return n;
  const [, pre = '', lib, rest] = m; const r = rest.split('::'); const last = r.pop(); const mid = r.join('');
  if (last.startsWith('__')) return '__' + lib + mid + last.slice(2);
  return pre + lib + mid + last;
};
const mkw = new Map(); for (const l of rd('projects/mkwii-reference/MAP.txt').split('\n')) { const [a, n] = l.trim().split(/\s+/); if (a && n && !n.startsWith('0x')) mkw.set(a.toUpperCase(), n); }
const opucName = new Map(); const starts = new Set();
for (const l of rd('projects/opuc1/MAP.txt').split('\n')) {
  const [a, n] = l.trim().split(/\s+/); if (!a || a.startsWith('#')) continue; starts.add(a.toUpperCase());
  if (n && !n.startsWith('0x')) opucName.set(a.toUpperCase(), n.startsWith('g_') ? norm(n.slice(2).replace(/__/g, '::').replace(/^::/, '__')) : n);
}
const firstStart = Math.min(...[...starts].map(a => parseInt(a, 16)));
const nativeEntries = new Map();
for (const l of rd('projects/opuc1/native_entries.tsv').split('\n')) { if (!l.trim() || l.startsWith('#')) continue; const [a, k, note] = l.split('\t'); nativeEntries.set(a.trim().toUpperCase(), { kind: (k ?? '').trim(), note: (note ?? '').trim(), used: false }); }
const nativeProblem = (addr, e) => {
  const v = parseInt(addr, 16);
  if (e.kind === 'slot') return v < firstStart ? null : 'native_entries.tsv slot is not below the first MAP entry';
  if (e.kind === 'thunk') return v > firstStart && v % 4 === 0 ? null : 'native_entries.tsv thunk is not a 4-aligned address inside a MAP entry';
  return `native_entries.tsv kind '${e.kind}' is not slot or thunk`;
};
const overrides = new Map();
for (const l of rd('projects/opuc1/rebind_overrides.tsv').split('\n')) { if (!l.trim() || l.startsWith('#')) continue; const [a, t, note] = l.split('\t'); overrides.set(a.toUpperCase(), { t: t.trim(), note: (note ?? '').trim() }); }

const files = [];
const walk = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (/\.(cpp|h|hpp|inc)$/.test(e.name)) files.push(p); } };
walk(path.join(root, 'runtime/src'));
const re = /(?<![A-Za-z_])(PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_NATIVE_(?:FUNCTION(?:_AS)?|WRAPPER_AS|REPLACEMENT_AS))\(\s*(?:0x)?([0-9A-Fa-f]{8})(?: \/\*mkw:([0-9A-Fa-f]{8})\*\/)?\s*,\s*([A-Za-z_][A-Za-z0-9_]*)/g;
const rows = []; const seen = new Map(); let bad = 0;
for (const f of files.sort()) {
  const rel = path.relative(root, f).split(path.sep).join('/');
  const src = fs.readFileSync(f, 'utf8');
  for (const m of src.matchAll(re)) {
    if (/^\s*#\s*define/.test(src.slice(src.lastIndexOf('\n', m.index) + 1, m.index + 1))) continue;
    const [, macro, addrRaw, mkwRaw, sym] = m; const addr = addrRaw.toUpperCase(); const mk = mkwRaw?.toUpperCase();
    const problems = [];
    if (!mk) problems.push('no /*mkw:*/ marker (OPUC-native registration: check by hand)');
    const ne = !mk && !starts.has(addr) ? nativeEntries.get(addr) : undefined;
    if (ne) { ne.used = true; const p = nativeProblem(addr, ne); if (p) problems.push(p); }
    else if (!starts.has(addr)) problems.push('not a function start in MAP.txt');
    const ov = mk && overrides.get(mk);
    const want = mk ? norm(mkw.get(mk) ?? '') : '';
    const have = opucName.get(addr) ?? '';
    let evidence;
    if (ov && ov.t.toUpperCase() === addr) evidence = `override: ${ov.note}`;
    else if (want && have === want) evidence = `name: ${have}`;
    else if (mk) problems.push(`name mismatch: OPUC '${have || '?'}' vs MKWii '${want || '?'}'`);
    if (seen.has(addr)) problems.push(`also bound at ${seen.get(addr)}`); else seen.set(addr, `${rel} ${sym}`);
    const okNative = !mk && problems.length === 1 && (starts.has(addr) || !!ne);
    if (okNative && ne) evidence = `${ne.kind}: ${ne.note}`;
    if (problems.length && !okNative) bad++;
    rows.push({ addr, mk: mk ?? '-', sym, file: rel, ok: !problems.length || okNative, evidence: evidence ?? problems.join('; ') });
  }
}
if (process.argv.includes('--list')) for (const r of rows.sort((a, b) => a.addr.localeCompare(b.addr))) console.log(`${r.ok ? 'ok ' : 'BAD'} ${r.addr} mkw:${r.mk} ${r.sym} ${r.file} :: ${r.evidence}`);
for (const [a, e] of nativeEntries) if (!e.used) { bad++; console.log(`BAD ${a} native_entries.tsv entry with no OPUC-native registration`); }
console.log(JSON.stringify({ bindings: rows.length, ok: rows.filter(r => r.ok).length, bad }));
process.exit(bad ? 1 : 0);
