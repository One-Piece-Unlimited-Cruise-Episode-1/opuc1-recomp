// Measures where One Piece: Unlimited Recompilation Episode 1 stands and rewrites the metrics block in STATUS.md.
//   node tools/status.mjs [--json] [--no-write]
// Every number comes from a file the pipeline wrote, so two sessions reading this agree on the state.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const p = (...x) => path.join(root, ...x);
const read = f => (fs.existsSync(p(f)) ? fs.readFileSync(p(f), 'utf8') : null);
const m = {};

const map = read('projects/opuc1/map_report.json');
if (map) { const j = JSON.parse(map); m.mapStarts = j.starts; m.mapNamed = j.named; m.mapSdkNamed = j.bySrc?.dsy ?? 0; }

const rb = read('projects/opuc1/rebind_report.md');
if (rb) {
  const num = re => Number((rb.match(re) || [])[1] ?? NaN);
  m.rebindMappedDistinct = num(/mapped sites: \d+ \((\d+) distinct/);
  m.rebindUnbound = num(/unbound registrations[^:]*: (\d+)/);
  m.rebindTodoDistinct = num(/TODO address uses[^:]*: \d+ \((\d+) distinct/);
}

const tl = read('build/logs/translate.log');
if (tl) {
  m.translatedFunctions = Number((tl.match(/Recursively translated (\d+) functions/) || [])[1] ?? NaN);
  m.translateOk = /Translation completed/.test(tl);
}
const cl = read('build/logs/compile.log');
if (cl) {
  m.compileErrors = (cl.match(/ error: /g) || []).length;
  m.compileLastLine = cl.trim().split('\n').slice(-1)[0].slice(0, 160);
}
const exeDir = p('build/native');
const exes = fs.existsSync(exeDir) ? fs.readdirSync(exeDir).filter(f => f.endsWith('.exe')) : [];
m.executables = exes.filter(f => !/_tests?.exe$|probe/.test(f));

// Boot milestones from the last run log (tools/run.ps1 writes build/logs/run.log).
const rl = read('build/logs/run.log');
// Each milestone needs an explicit marker in the run log, never a loose word (aurora prints "present mode" at
// startup). "[opuc] milestone <name>" lines are written by the runtime (runtime/src/hle/opuc/milestones.cpp).
const milestones = [
  ['M2 guest code ran OSInit', /OSExceptionInit: skipped exception vector setup/],
  ['M2 reached main (ALSystem::Main 803F03D8)', /\[opuc\] milestone main\b/],
  ['M3 first GX draw', /\[opuc\] milestone first-draw\b/],
  ['M3 first GXCopyDisp (not yet a presented frame)', /\[opuc\] milestone frame\b/],
  ['M4 disc file read', /\[opuc\] milestone disc-read\b/],
];
if (rl) m.runMilestones = milestones.filter(([, re]) => re.test(rl)).map(([n]) => n);
if (rl) m.runFatal = ((rl.match(/FATAL[^\r\n]*|Runtime error:[^\r\n]*|InvokeIndirect\w*: target[^\r\n]*/) || [''])[0].trim()) || null;

const block = ['<!-- metrics:begin (tools/status.mjs) -->', '```json', JSON.stringify({ measured: new Date().toISOString(), ...m }, null, 2), '```', '<!-- metrics:end -->'].join('\n');
if (!process.argv.includes('--no-write')) {
  const sp = p('STATUS.md'); let s = fs.readFileSync(sp, 'utf8');
  s = s.replace(/<!-- metrics:begin[\s\S]*?<!-- metrics:end -->/, block);
  fs.writeFileSync(sp, s);
}
console.log(process.argv.includes('--json') ? JSON.stringify(m) : block);
