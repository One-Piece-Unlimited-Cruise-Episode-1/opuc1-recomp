// Re-points the vendored upstream runtime (written for Mario Kart Wii PAL) at OPUC addresses.
//
// The upstream runtime names guest code by MKWii PAL address in five forms:
//   PPC_NATIVE_OVERRIDE[_VOID](ADDR, sym, ...)    HLE replacement, read by the translator (skips ADDR)
//   REGISTER_NATIVE_*(0xADDR, sym)                 HLE replacement, read by the translator
//   MKW_GUEST_FUNC(ADDR)                           native code calling translated guest code
//   MKW_GADDR(ADDR) / MKW_GADDR_CT(ADDR)           guest function or data address used by HLE code
// For each address this tool looks up the MKWii symbol (projects/mkwii-reference/MAP.txt), normalizes it to the
// RVL SDK spelling and looks that up in the OPUC map (projects/opuc1/MAP.txt).
//   mapped     -> the address is replaced and a /*mkw:ADDR*/ marker is left behind (re-runs skip marked sites)
//   unmapped registration -> OPUC_UNBOUND(MACRO, ADDR, ...) which compiles to nothing, so the translated SDK
//                            code at OPUC's address runs instead (GX falls back to the FIFO path)
//   unmapped address use  -> OPUC_TODO(MACRO, ADDR) (fatal at run time when reached, listed in the report)
// Re-running after the OPUC map or the overrides improve resolves OPUC_TODO / OPUC_UNBOUND sites back into the
// original macro, so the vendored runtime can be rebound incrementally.
// Overrides: projects/opuc1/rebind_overrides.tsv ("MKWADDR<TAB>OPUCADDR|unbound<TAB>note") wins over name matching.
//
// usage: node rebind-runtime.mjs <Recomp root> [--dry-run]
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(process.argv[2] ?? '.');
const dry = process.argv.includes('--dry-run');
const rd = p => fs.readFileSync(path.join(root, p), 'utf8');

const norm = n => {
  n = n.replace(/^RVL::/, '');
  const m = n.match(/^(__)?([A-Za-z0-9]+)::(.*)$/); if (!m) return n;
  const [, pre = '', lib, rest] = m; const r = rest.split('::'); const last = r.pop(); const mid = r.join('');
  if (last.startsWith('__')) return '__' + lib + mid + last.slice(2);
  return pre + lib + mid + last;
};
const mkw = new Map();
for (const l of rd('projects/mkwii-reference/MAP.txt').split('\n')) {
  const [a, n] = l.trim().split(/\s+/); if (a && n && !n.startsWith('0x')) mkw.set(a.toUpperCase(), n);
}
const opuc = new Map();
for (const l of rd('projects/opuc1/MAP.txt').split('\n')) {
  const [a, n] = l.trim().split(/\s+/); if (!a || !n || a.startsWith('#') || n.startsWith('0x')) continue;
  let k = n; if (k.startsWith('g_')) k = norm(k.slice(2).replace(/__/g, '::').replace(/^::/, '__'));
  if (!opuc.has(k)) opuc.set(k, a.toUpperCase());
}
const overrides = new Map();
const ovPath = path.join(root, 'projects/opuc1/rebind_overrides.tsv');
if (fs.existsSync(ovPath)) for (const l of fs.readFileSync(ovPath, 'utf8').split('\n')) {
  if (!l.trim() || l.startsWith('#')) continue; const [a, t] = l.split('\t'); overrides.set(a.toUpperCase(), t.trim());
}
const resolve = a => {
  a = a.toUpperCase();
  if (overrides.has(a)) { const t = overrides.get(a); return t === 'unbound' ? { addr: null, name: mkw.get(a) ?? '', how: 'override' } : { addr: t.toUpperCase(), name: mkw.get(a) ?? '', how: 'override' }; }
  const name = mkw.get(a); if (!name) return { addr: null, name: '', how: 'no-mkw-symbol' };
  if (/_(switch|caseD)/.test(name)) return { addr: null, name, how: 'mkw-jump-table-piece' };
  const o = opuc.get(norm(name)); return o ? { addr: o, name, how: 'name' } : { addr: null, name, how: 'no-opuc-match' };
};

const files = [];
const walk = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) { if (e.name !== 'third_party') walk(p); } else if (/\.(c|cc|cpp|h|hpp|inc)$/.test(e.name)) files.push(p); } };
for (const d of ['runtime/src', 'runtime/include', 'runtime/game']) walk(path.join(root, d));
// runtime/src/hle/opuc/ holds OPUC-native HLE written against OPUC addresses: never rebind it.
for (let i = files.length - 1; i >= 0; i--) if (/[\\/]hle[\\/]opuc[\\/]/.test(files[i])) files.splice(i, 1);

const report = { mapped: [], unbound: [], todo: [] };
// Every site, in any of its four spellings, is normalized to (macro, 0x-prefix, MKWii address) and re-emitted from
// the current map + overrides, so reruns converge:
//   pristine   MACRO(ADDR            mapped   MACRO(OPUC /*mkw:ADDR*/
//   unbound    OPUC_UNBOUND(MACRO, ADDR      todo     OPUC_TODO(MACRO, ADDR
const REG = 'PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_NATIVE_(?:FUNCTION(?:_AS)?|WRAPPER_AS|REPLACEMENT_AS)';
const USE = 'MKW_GUEST_FUNC|MKW_GADDR_CT|MKW_GADDR';
const H = '[0-9A-Fa-f]{8}';
const site = new RegExp(
  `OPUC_UNBOUND[(](${REG}), ((?:0x)?)(${H})` +                       // 1 macro, 2 zx, 3 mkw
  `|OPUC_TODO[(](${USE}), (${H})` +                                 // 4 macro, 5 mkw
  `|(?<![A-Za-z_])(${REG}|${USE})([ ]*[(][ ]*)((?:0x)?)(${H})(?: /[*]mkw:(${H})[*]/)?`, // 6 macro, 7 open, 8 zx, 9 addr, 10 mkw
  'g');
const isReg = m => /^(PPC_NATIVE_OVERRIDE|REGISTER_NATIVE_)/.test(m);
const boundOpuc = new Map(); // OPUC address -> first registration site (a guest address can only have one native)
files.sort();
let changedFiles = 0;
for (const f of files) {
  const rel = path.relative(root, f).split(path.sep).join('/');
  const orig = fs.readFileSync(f, 'utf8');
  const src = orig.split('\n').map(line => {
    if (/^\s*#\s*define/.test(line)) return line;
    return line.replace(site, (m, m1, z1, a1, m4, a5, m6, open7, z8, a9, k10) => {
      let macro, zx, mkwAddr;
      if (m1) { macro = m1; zx = z1; mkwAddr = a1; }
      else if (m4) { macro = m4; zx = ''; mkwAddr = a5; }
      else { macro = m6; zx = z8; mkwAddr = k10 ?? a9; }
      mkwAddr = mkwAddr.toUpperCase();
      const res = resolve(mkwAddr);
      if (res.addr && isReg(macro)) {
        const prev = boundOpuc.get(res.addr);
        if (prev && prev !== rel + ':' + mkwAddr) {
          report.unbound.push({ mkw: mkwAddr, name: res.name, macro, file: rel, why: `duplicate of ${prev} at ${res.addr}` });
          return `OPUC_UNBOUND(${macro}, ${zx}${mkwAddr}`;
        }
        boundOpuc.set(res.addr, rel + ':' + mkwAddr);
      }
      if (res.addr) {
        report.mapped.push({ mkw: mkwAddr, opuc: res.addr, name: res.name, macro, file: rel, how: res.how });
        return `${macro}(${zx}${res.addr} /*mkw:${mkwAddr}*/`;
      }
      if (isReg(macro)) { report.unbound.push({ mkw: mkwAddr, name: res.name, macro, file: rel, why: res.how }); return `OPUC_UNBOUND(${macro}, ${zx}${mkwAddr}`; }
      report.todo.push({ mkw: mkwAddr, name: res.name, macro, file: rel, why: res.how });
      return `OPUC_TODO(${macro}, ${mkwAddr}`;
    });
  }).join('\n');
  if (src !== orig) { changedFiles++; if (!dry) fs.writeFileSync(f, src); }
}
const uniq = (arr, k) => [...new Map(arr.map(x => [k(x), x])).values()];
const lines = ['# OPUC runtime rebind report', '', 'Generated by `tools/rebind-runtime.mjs`. Upstream MKWii PAL address -> OPUC address.', '',
  `- mapped sites: ${report.mapped.length} (${uniq(report.mapped, x => x.mkw).length} distinct addresses)`,
  `- unbound registrations (translated SDK code runs instead): ${report.unbound.length}`,
  `- TODO address uses (OPUC_TODO, fatal when reached): ${report.todo.length} (${uniq(report.todo, x => x.mkw).length} distinct)`, '',
  '## TODO address uses', '', '| MKWii | symbol | macro | file | why |', '|---|---|---|---|---|',
  ...uniq(report.todo, x => x.mkw + x.file).map(x => `| ${x.mkw} | ${x.name || '?'} | ${x.macro} | ${x.file} | ${x.why} |`), '',
  '## Unbound registrations', '', '| MKWii | symbol | macro | file | why |', '|---|---|---|---|---|',
  ...report.unbound.map(x => `| ${x.mkw} | ${x.name || '?'} | ${x.macro} | ${x.file} | ${x.why} |`), '',
  '## Mapped', '', '| MKWii | OPUC | symbol | how |', '|---|---|---|---|',
  ...uniq(report.mapped, x => x.mkw).sort((a, b) => a.opuc.localeCompare(b.opuc)).map(x => `| ${x.mkw} | ${x.opuc} | ${x.name} | ${x.how} |`), ''];
if (!dry) fs.writeFileSync(path.join(root, 'projects/opuc1/rebind_report.md'), lines.join('\n'));
console.log(JSON.stringify({ files: files.length, changedFiles, mapped: report.mapped.length, mappedDistinct: uniq(report.mapped, x => x.mkw).length, unbound: report.unbound.length, todo: report.todo.length, todoDistinct: uniq(report.todo, x => x.mkw).length, dry }));
