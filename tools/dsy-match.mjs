// Name functions in main.dol by matching Dolphin's totaldb.dsy hash signatures.
// usage: node dsy-match.mjs <main.dol> <totaldb.dsy> <functions.txt> <out.txt>
import fs from 'node:fs';
const [dolPath, dsyPath, funcPath, outPath] = process.argv.slice(2);
const dol = fs.readFileSync(dolPath);
const secs = [];
for (let i = 0; i < 7; i++) {
  const off = dol.readUInt32BE(i * 4), addr = dol.readUInt32BE(0x48 + i * 4), size = dol.readUInt32BE(0x90 + i * 4);
  if (size) secs.push({ off, addr, size });
}
const rd = a => { for (const s of secs) if (a >= s.addr && a < s.addr + s.size) return dol.readUInt32BE(s.off + a - s.addr); return null; };
function checksum(start, size) {
  let sum = 0;
  for (let a = start; a < start + size; a += 4) {
    const opcode = rd(a); if (opcode === null) return null;
    const op = (opcode & 0xFC000000) >>> 0; let op2 = 0, op3 = 0; const aux = op >>> 26;
    switch (aux) {
      case 4: op2 = opcode & 0x3F; if ([0, 8, 16, 21, 22].includes(op2)) op3 = opcode & 0x7C0; break;
      case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15: op2 = opcode & 0x03FF0000; break;
      case 19: case 31: case 63: op2 = opcode & 0x7FF; break;
      case 59: op2 = opcode & 0x3F; if (op2 < 16) op3 = opcode & 0x7C0; break;
      default: if (aux >= 32 && aux < 56) op2 = opcode & 0x03FF0000;
    }
    sum = (((sum << 17) & 0xFFFE0000) | ((sum >>> 15) & 0x1FFFF)) >>> 0;
    sum = (sum ^ (op | op2 | op3)) >>> 0;
  }
  return sum;
}
const dsy = fs.readFileSync(dsyPath);
const n = dsy.readUInt32LE(0); const db = new Map(); const sizes = new Set();
for (let i = 0; i < n; i++) {
  const o = 4 + i * 136; const cs = dsy.readUInt32LE(o), sz = dsy.readUInt32LE(o + 4);
  const name = dsy.toString('latin1', o + 8, o + 136).replace(/\0.*$/s, '');
  const k = cs + ':' + sz; if (!db.has(k)) db.set(k, []); db.get(k).push(name); sizes.add(sz);
}
const sizeList = [...sizes].sort((a, b) => a - b);
const funcs = fs.readFileSync(funcPath, 'utf8').trim().split('\n').map(l => { const [a, ...r] = l.split(' '); return { a: parseInt(a, 16), n: r.join(' ') }; });
const out = []; let hits = 0, ambiguous = 0;
for (let i = 0; i < funcs.length; i++) {
  const f = funcs[i]; const next = i + 1 < funcs.length ? funcs[i + 1].a : f.a + 0x4000; const span = next - f.a;
  const found = new Set();
  for (const sz of sizeList) {
    if (sz > span) break; if (sz < span - 64) continue;
    const cs = checksum(f.a, sz); const k = cs + ':' + sz; if (db.has(k)) for (const nm of db.get(k)) found.add(nm);
  }
  if (found.size) { hits++; if (found.size > 1) ambiguous++; out.push(`${f.a.toString(16)} ${[...found].join('|')} ; ghidra=${f.n}`); }
}
fs.writeFileSync(outPath, out.join('\n') + '\n');
console.log(`dsy entries=${n} functions=${funcs.length} matched=${hits} ambiguous=${ambiguous}`);
