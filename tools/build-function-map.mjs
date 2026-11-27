// Builds the translator's function map (one "hexaddr name" line per function start) for OPUC main.dol.
//
// Sources, in priority order:
//   1. CodeWarrior register save/restore families (_save_gpr_N, _rest_gpr_N, _save_fpr_N, _rest_fpr_N), found by
//      instruction pattern in main.dol. The translator looks these up by name (FunctionMap.NumberedFamily).
//   2. Fixed boot symbols (__start, __init_registers, __init_data, memcpy, ...).
//   3. RVL SDK names from Dolphin's totaldb.dsy signature matches (dsy-match.mjs), only inside the SDK span and
//      only when the match is unique. These are the names the runtime binds HLE natives to.
//   4. Ghidra names from the opuc1 project (ghidra_functions.txt), sanitized to C identifiers.
// Unnamed starts are written as "addr 0xaddr" (the translator's placeholder form).
//
// usage: node build-function-map.mjs <main.dol> <ghidra_functions.txt> <dsy_matches.txt> <out MAP.txt> [report.json]
import fs from 'node:fs';

const [dolPath, ghidraPath, dsyPath, outPath, reportPath] = process.argv.slice(2);
const SDK_LO = 0x8026a000, SDK_HI = 0x80300000;

const dol = fs.readFileSync(dolPath);
const text = [];
for (let i = 0; i < 7; i++) {
  const off = dol.readUInt32BE(i * 4), addr = dol.readUInt32BE(0x48 + i * 4), size = dol.readUInt32BE(0x90 + i * 4);
  if (size) text.push({ off, addr, size });
}
const rd = a => { for (const s of text) if (a >= s.addr && a < s.addr + s.size) return dol.readUInt32BE(s.off + a - s.addr); return null; };

const names = new Map();   // addr -> {name, src}
const starts = new Set();
const put = (a, name, src, force = false) => { starts.add(a); if (force || !names.has(a)) names.set(a, { name, src }); };

// 1. save/restore families. stw rN,-4*(32-N)(r11) = 0x90000000 | N<<21 | 11<<16 | (-4*(32-N) & 0xffff)
const fam = [
  { prefix: '_save_gpr_', base: 0x90000000, step: 4, lo: 14 },   // stw
  { prefix: '_rest_gpr_', base: 0x80000000, step: 4, lo: 14 },   // lwz
  { prefix: '_save_fpr_', base: 0xD8000000, step: 8, lo: 14 },   // stfd
  { prefix: '_rest_fpr_', base: 0xC8000000, step: 8, lo: 14 },   // lfd
];
const famFound = {};
for (const s of text) {
  for (let a = s.addr; a + 4 * 19 <= s.addr + s.size; a += 4) {
    for (const f of fam) {
      let ok = true;
      for (let n = f.lo; n <= 31 && ok; n++) {
        const disp = (-(f.step) * (32 - n)) & 0xffff;
        const want = (f.base | (n << 21) | (11 << 16) | disp) >>> 0;
        if (rd(a + 4 * (n - f.lo)) !== want) ok = false;
      }
      if (ok && rd(a + 4 * (32 - f.lo)) === 0x4e800020) {
        famFound[f.prefix] = a.toString(16);
        for (let n = f.lo; n <= 31; n++) put(a + 4 * (n - f.lo), f.prefix + n, 'pattern', true);
      }
    }
  }
}

// 2. fixed boot symbols (checked against Ghidra: Boot::Start @ 8000403c is the DOL entry)
const fixed = { 0x8000403c: '__start', 0x800041a8: '__init_registers', 0x80004238: '__init_data', 0x80004338: 'memcpy', 0x80004388: '__fill_mem' };
for (const [a, n] of Object.entries(fixed)) put(Number(a), n, 'fixed', true);

// 2b. manual SDK names (projects/opuc1/sdk_names.tsv next to the output map): ADDR<TAB>Name<TAB>evidence
const manualPath = outPath.replace(/[^/\\]+$/, 'sdk_names.tsv');
if (fs.existsSync(manualPath)) {
  for (const line of fs.readFileSync(manualPath, 'utf8').split('\n')) {
    if (!line.trim() || line.startsWith('#')) continue;
    const [a, n] = line.split('\t'); if (!/^[0-9a-fA-F]{8}$/.test(a) || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(n ?? '')) continue;
    const addr = parseInt(a, 16); if (names.get(addr)?.src === 'pattern') continue;
    put(addr, n, 'manual', true);
  }
}

// 3. SDK names from Dolphin signatures
//    A signature is a hash of masked opcodes, so short functions collide: Dolphin named 80283034
//    (VISetPreRetraceCallback) and three other addresses AIRegisterDMACallback. A name that lands on more than one
//    address is therefore ambiguous and dropped everywhere; sdk_names.tsv settles it.
let sdkNamed = 0, sdkAmbiguousNames = 0;
const ident = /^[A-Za-z_][A-Za-z0-9_]*$/;
const manualNames = new Set([...names.values()].filter(v => v.src === 'manual').map(v => v.name));
const dsyCandidates = [];
for (const line of fs.readFileSync(dsyPath, 'utf8').split('\n')) {
  const m = line.match(/^([0-9a-f]{8}) (.*?) ; ghidra=/); if (!m) continue;
  const a = parseInt(m[1], 16); if (a < SDK_LO || a >= SDK_HI) continue;
  if (m[2].includes('|')) continue;
  const nm = m[2].split(/\s/)[0];
  if (!ident.test(nm) || /^(J[A-Z0-9]|_restgpr|_savegpr)/.test(nm)) continue;
  dsyCandidates.push({ a, nm });
}
const dsyNameCount = new Map();
for (const { nm } of dsyCandidates) dsyNameCount.set(nm, (dsyNameCount.get(nm) ?? 0) + 1);
sdkAmbiguousNames = [...dsyNameCount.values()].filter(n => n > 1).length;
for (const { a, nm } of dsyCandidates) {
  if (dsyNameCount.get(nm) > 1) continue;
  if (names.has(a) && names.get(a).src !== 'ghidra') continue;
  if (manualNames.has(nm)) continue; // a manual name wins over a dsy duplicate
  put(a, nm, 'dsy', true); sdkNamed++;
}

// 4. Ghidra names
const seenGhidra = new Set();
for (const line of fs.readFileSync(ghidraPath, 'utf8').split('\n')) {
  const m = line.match(/^([0-9a-f]{8}) (.+)$/); if (!m) continue;
  const a = parseInt(m[1], 16); starts.add(a);
  if (/^(FUN_|thunk_FUN_)/.test(m[2]) || names.has(a)) continue;
  let nm = m[2].replace(/::/g, '__').replace(/[^A-Za-z0-9_]/g, '_');
  if (!/^[A-Za-z_]/.test(nm)) nm = '_' + nm;
  // keep game names out of the SDK namespace so a game function can never bind an SDK native by accident
  nm = 'g_' + nm;
  if (seenGhidra.has(nm)) nm = nm + '_' + m[1];
  seenGhidra.add(nm);
  names.set(a, { name: nm, src: 'ghidra' });
}

// 5. Function pointers stored in data sections (vtables, callback tables, SDK defaults such as the default
//    switch-thread callback 802746a4) whose targets no other source lists. The translator dispatches indirect
//    calls only to known starts, so every such target must be in the map. A target qualifies when it is not a
//    known start, it lies in text, and it looks like a function entry: the previous word ends a function (blr,
//    rfi, unconditional b, or zero padding) or the target is a prologue (stwu r1 / mflr r0). Runs of pointers that
//    all land between the same two known starts are switch jump tables and are skipped.
const dataSecs = [];
for (let i = 9; i < 18; i++) {   // D2.. only: D0/D1 are extab/extabindex (EH landing pads are mid-function)
  const off = dol.readUInt32BE(i * 4), addr = dol.readUInt32BE(0x48 + i * 4), size = dol.readUInt32BE(0x90 + i * 4);
  if (size) dataSecs.push({ off, addr, size });
}
const inText = a => text.some(s => a >= s.addr && a < s.addr + s.size);
let known = [...starts].sort((x, y) => x - y);
const owner = a => { let lo = 0, hi = known.length - 1, r = -1; while (lo <= hi) { const m = (lo + hi) >> 1; if (known[m] <= a) { r = m; lo = m + 1; } else hi = m - 1; } return r; };
const looksLikeEntry = a => {
  const prev = rd(a - 4), cur = rd(a);
  if (cur === null || cur === 0 || (cur >>> 26) === 0) return false;   // padding / primary opcode 0 is not code
  if ((cur & 0xFFFF0000) === 0x94210000 || cur === 0x7C0802A6) return true;      // stwu r1,-x(r1) / mflr r0
  return prev === 0x4E800020 || prev === 0x4E800420 || prev === 0x4C000064 || prev === 0 || ((prev & 0xFC000003) === 0x48000000); // blr / bctr / rfi / pad / b
};
const dataSeeds = [];
// Iterate to a fixpoint: every accepted start splits an owner, which can make a neighbour's tail-call `b` leave
// its (now smaller) owner, e.g. 80380000 then 8038000c.
for (let round = 0; round < 6; round++) {
const before = dataSeeds.length;
for (const s of dataSecs) {
  const words = [];
  for (let o = 0; o + 4 <= s.size; o += 4) words.push({ at: s.addr + o, v: dol.readUInt32BE(s.off + o) });
  for (let i = 0; i < words.length; i++) {
    const { v } = words[i];
    if ((v & 3) || !inText(v) || starts.has(v)) continue;
    // jump-table run: a neighbour points into the same known function
    const ow = owner(v);
    const same = j => j >= 0 && j < words.length && inText(words[j].v) && !(words[j].v & 3) && owner(words[j].v) === ow;
    if (same(i - 1) || same(i + 1)) {
      // Inside a run that lands in one known function this is usually a switch table (case labels follow a
      // branch inside the function). Keep the target only when it is clearly a separate entry: a prologue, or the
      // previous word is blr or an unconditional b that leaves the owning function (tail-call thunk, e.g. the
      // memcpy/memclear thunks at 80358640/80358644 stored in a function-pointer table).
      const cur = rd(v), prev = rd(v - 4);
      const ownerEnd = ow + 1 < known.length ? known[ow + 1] : 0xFFFFFFFF;
      const bTarget = (prev & 0xFC000003) === 0x48000000 ? (v - 4 + (((prev & 0x03FFFFFC) ^ 0x02000000) - 0x02000000)) >>> 0 : null;
      const separate = (cur & 0xFFFF0000) === 0x94210000 || cur === 0x7C0802A6 || prev === 0x4E800020 || prev === 0x4E800420 ||
        (bTarget !== null && (bTarget < known[ow] || bTarget >= ownerEnd));
      if (!separate) continue;
    }
    if (!looksLikeEntry(v)) continue;
    dataSeeds.push(v); starts.add(v);
  }
}
known = [...starts].sort((x, y) => x - y);
if (dataSeeds.length === before) break;
}
for (const a of new Set(dataSeeds)) put(a, '0x' + a.toString(16).padStart(8, '0'), 'data-pointer');
for (const a of new Set(dataSeeds)) names.delete(a);   // unnamed: written as the address placeholder

// 6. Code addresses materialized in code with lis rX,hi + addi/ori rY,rX,lo (callbacks handed to registration
//    functions, e.g. ALNode::RegisterDefaults passing the "Position" accessor 803adc04). Same entry test as above.
const codeSeeds = [];
for (const s of text) {
  for (let a = s.addr; a < s.addr + s.size; a += 4) {
    const w = rd(a);
    if ((w >>> 26) !== 15 || ((w >>> 16) & 31) !== 0) continue;          // lis rD (addis rD,0,imm)
    const rD = (w >>> 21) & 31, hi = (w & 0xFFFF) << 16;
    for (let k = 1; k <= 8; k++) {
      const u = rd(a + 4 * k); if (u === null) break;
      const op = u >>> 26, rA = (u >>> 16) & 31, rT = (u >>> 21) & 31;
      let v = null;
      // addi rT,rD,simm, and addic / addic. rT,rD,simm: CodeWarrior forms a function's address with addic. when it
      // also tests it for null (`if (&fn)`), e.g. the ShaderCore pass-shader thunk 8011a498 (lis r0,0x8012 +
      // addic. r0,r0,-0x5b68 at 801c3060 and 801f49c8), which nothing calls directly.
      if ((op === 14 || op === 12 || op === 13) && rA === rD) v = (hi + ((u & 0xFFFF) << 16 >> 16)) >>> 0;
      else if (op === 24 && rT === rD) v = (hi | (u & 0xFFFF)) >>> 0;            // ori rA,rD,uimm (rS=rD)
      if (v !== null) { if (!(v & 3) && inText(v) && !starts.has(v) && looksLikeEntry(v)) {
          const prev = rd(v - 4), cur = rd(v);
          const strong = (cur & 0xFFFF0000) === 0x94210000 || cur === 0x7C0802A6 || prev === 0x4E800020 || prev === 0x4E800420;
          if (strong) codeSeeds.push(v);
        }
        break; }
      // stop when rD is overwritten
      if (((u >>> 21) & 31) === rD && op !== 36 && op !== 37 && op !== 38 && op !== 44 && op !== 45 && op !== 47) break;
    }
  }
}
for (const a of new Set(codeSeeds)) put(a, '0x' + a.toString(16).padStart(8, '0'), 'code-constant');
for (const a of new Set(codeSeeds)) names.delete(a);

const sorted = [...starts].sort((x, y) => x - y);
const out = ['# OPUC1 PAL (ROUPAF) main.dol function map. Generated by tools/build-function-map.mjs; do not edit.'];
for (const a of sorted) { const h = a.toString(16).padStart(8, '0'); const n = names.get(a); out.push(`${h} ${n ? n.name : '0x' + h}`); }
fs.writeFileSync(outPath, out.join('\n') + '\n');
const bySrc = {}; for (const v of names.values()) bySrc[v.src] = (bySrc[v.src] || 0) + 1;
const report = { starts: sorted.length, named: names.size, bySrc, sdkNamed, sdkAmbiguousNames, dataPointerSeeds: new Set(dataSeeds).size, codeConstantSeeds: new Set(codeSeeds).size, families: famFound };
if (reportPath) fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report));
