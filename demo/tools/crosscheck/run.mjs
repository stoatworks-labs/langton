// The JavaScript half of the browser demo's cross-check: the PORT block of
// demo/plugin.js (between its BEGIN PORT and END PORT markers, sliced out as
// text and run here, so what is checked is exactly what the page runs), driven
// through printer.cpp's scenario and printed the same way. crosscheck.sh diffs
// the two outputs. Read this beside printer.cpp: each block mirrors one there.
//
//     node demo/tools/crosscheck/run.mjs > js.txt

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, '..', '..', 'plugin.js'), 'utf8');
const begin = source.indexOf('// ==== BEGIN PORT');
const end = source.indexOf('// ==== END PORT');
if (begin < 0 || end < begin) {
  process.stderr.write('run.mjs: the PORT markers are missing from demo/plugin.js\n');
  process.exit(2);
}
const names = [
  'Pcg', 'CellHash', 'SeedSalt', 'ParseTurns', 'World', 'kRules', 'kRuleCount', 'kMaxAnts', 'StepClock',
  'SpeedFromParam', 'ParamFromSpeed', 'DensityFromParam', 'RowsFromParam', 'ParamFromRows', 'GapFromParam',
  'TrailFromParam', 'ParamFromTrail', 'ThresholdU32', 'GridFor', 'OptionIndex', 'kMaxRoundsPerFrame', 'lround',
];
// eslint-disable-next-line no-new-func
const P = new Function(`${source.slice(begin, end)}\nreturn { ${names.join(', ')} };`)();

const out = [];
const print = (line) => out.push(line);

const view = new DataView(new ArrayBuffer(8));
const h32 = (v) => (v >>> 0).toString(16).padStart(8, '0');
const f32 = (v) => { view.setFloat32(0, v); return h32(view.getUint32(0)); };
const f64 = (v) => { view.setFloat64(0, v); return h32(view.getUint32(0)) + h32(view.getUint32(4)); };
const F = Math.fround;

function ants(w) {
  const s = w.ants.map((a) => `${a.x},${a.y},${a.heading};`).join('');
  return s === '' ? '-' : s;
}

function state(label, w) {
  let hc = 2166136261;
  let hv = 2166136261;
  let ht = 2166136261;
  let nonzero = 0;
  let sum = 0;
  for (const c of w.colour) {
    hc = Math.imul((hc ^ c) >>> 0, 16777619) >>> 0;
    nonzero += c !== 0 ? 1 : 0;
    sum += c;
  }
  for (const v of w.visited) hv = Math.imul((hv ^ v) >>> 0, 16777619) >>> 0;
  for (const t of w.visit) ht = Math.imul((ht ^ t) >>> 0, 16777619) >>> 0;
  const dirty = w.TakeDirtyRows();
  print(`${label} ${w.cols}x${w.rows} n=${w.Colours()} rounds=${w.rounds} clock=${w.clock} c=${h32(hc)} v=${h32(hv)} t=${h32(ht)} nz=${nonzero} sum=${sum} ants=${ants(w)} dirty=${dirty ? `${dirty.first}..${dirty.last}` : 'none'}`);
}

function walk(w, label, chunks, reverse) {
  chunks.forEach((n, i) => {
    w.Step(n, reverse);
    state(`${label}${reverse ? ' back' : ' fwd'}${i}`, w);
  });
}

//---------------------------------------------------------------------------
// ParseTurns.
//---------------------------------------------------------------------------
for (const text of ['RL', 'rl', 'LLRR', 'R', '', 'LRX', 'LLLLLLLLLLLL', 'LLLLLLLLLLLLL', 'RlRl', ' RL']) {
  const s = P.ParseTurns(text).map((t) => (t > 0 ? '+' : '-')).join('');
  print(`parse '${text}' ${s === '' ? 'empty' : s}`);
}

//---------------------------------------------------------------------------
// Controls.
//---------------------------------------------------------------------------
for (let k = -10; k <= 1010; k += 1) {
  const v = F(k / 1000.0);
  print(`ctl ${k} ${f32(v)} speed=${f64(P.SpeedFromParam(v))} rows=${P.RowsFromParam(v)} trail=${f64(P.TrailFromParam(v))} gap=${f64(P.GapFromParam(v))} density=${f64(P.DensityFromParam(v))} thr=${h32(P.ThresholdU32(P.DensityFromParam(v)))}`);
}
for (const s of [600.0, 3000.0, 1.0, 1.0e6, 0.5, 2.0e6, 1234.5, 0.0, -3.0]) {
  print(`paramFromSpeed ${f64(s)} ${f32(P.ParamFromSpeed(s))} back=${f64(P.SpeedFromParam(P.ParamFromSpeed(s)))}`);
}
for (let r = 1; r <= 1100; r += 1) {
  print(`paramFromRows ${r} ${f32(P.ParamFromRows(r))} back=${P.RowsFromParam(P.ParamFromRows(r))}`);
}
for (const t of [3000.0, 10.0, 1.0e6, 5.0, 777.0, 0.0]) {
  print(`paramFromTrail ${f64(t)} ${f32(P.ParamFromTrail(t))} back=${f64(P.TrailFromParam(P.ParamFromTrail(t)))}`);
}
for (const p of [0.0, 1e-12, 0.2, 0.5, 1.0, 0.99999999999, 2 ** -33, 1.0 - 2 ** -33, -1.0, F(0.2), F(0.35), 2.0]) {
  print(`threshold ${f64(p)} ${h32(P.ThresholdU32(p))}`);
}
const rasters = [[1280, 720], [1920, 1080], [640, 360], [1024, 768], [720, 1280], [333, 777], [1, 1], [3840, 2160], [1366, 768], [100, 0]];
for (const rows of [1, 16, 17, 99, 120, 121, 135, 540, 1080]) {
  for (const [w, h] of rasters) {
    const g = P.GridFor(rows, w, h);
    print(`grid ${rows} ${w}x${h} -> ${g.cols}x${g.rows}`);
  }
}
for (const x of [-2.5, -1.5, -0.5, -0.49999999999999994, 0.49999999999999994, 0.5, 1.5, 2.5, -3.7, 3.7, -1e9 - 0.5]) {
  print(`lround ${f64(x)} ${P.lround(x)}`);
}
for (const v of [-1.5, -0.5, -0.49, 0.0, 0.49, 0.5, 0.51, 1.5, 2.5, 6.5, 7.0, 100.0].map(F)) {
  for (const count of [2, 3, 5, 7, 8]) print(`option ${f32(v)} ${count} -> ${P.OptionIndex(v, count)}`);
}

//---------------------------------------------------------------------------
// The hash, and the seed's salt.
//---------------------------------------------------------------------------
for (let i = 0; i < 256; i += 1) print(`pcg ${i} ${h32(P.Pcg(i))}`);
{
  let x = 0x12345678;
  for (let i = 0; i < 2000; i += 1) {
    x = P.Pcg(x);
    if (i % 50 === 0) print(`pcgchain ${i} ${h32(x)}`);
  }
}
for (const v of [0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe]) print(`pcg ${h32(v)} ${h32(P.Pcg(v))}`);
for (const x of [0, 1, 1919]) {
  for (const y of [0, 1, 1079]) {
    for (const salt of [0, 1, 0xdeadbeef]) print(`cellhash ${x} ${y} ${h32(salt)} ${h32(P.CellHash(x, y, salt))}`);
  }
}
for (const seed of [0.0, 1.0, 2.0, 99.0, 9999.0, 10000.0, -1.0, 1.5, 2.5, 0.49].map(F)) {
  for (const serial of [0, 1, 2, 0xffffffff]) print(`salt ${f32(seed)} ${h32(serial)} ${h32(P.SeedSalt(seed, serial))}`);
}

//---------------------------------------------------------------------------
// The step clock.
//---------------------------------------------------------------------------
{
  const speeds = [P.SpeedFromParam(P.ParamFromSpeed(600.0)), P.SpeedFromParam(P.ParamFromSpeed(3000.0)),
    1.0, 600.0, 1.0e6, P.SpeedFromParam(F(0.3)), 7.3];
  let caseIndex = 0;
  for (const speed of speeds) {
    for (const fps of [60.0, 23.976, 5.0, 144.0]) {
      for (const jitter of [false, true]) {
        const clock = new P.StepClock();
        let digest = 2166136261;
        let total = 0;
        for (let i = 0; i < 3000; i += 1) {
          let dt = 1.0 / fps;
          if (jitter) dt = dt * (0.5 + P.Pcg(i) / 4294967296.0);
          const whole = clock.Advance(speed, dt, P.kMaxRoundsPerFrame);
          total += whole;
          digest = Math.imul((digest ^ whole) >>> 0, 16777619) >>> 0;
          if (i % 250 === 0) print(`clock ${caseIndex} ${i} ${whole} ${f64(clock.Fraction())}`);
        }
        print(`clockcase ${caseIndex} speed=${f64(speed)} fps=${f64(fps)} jitter=${jitter ? 1 : 0} total=${total} digest=${h32(digest)} owed=${f64(clock.Fraction())}`);
        caseIndex += 1;
      }
    }
  }
  const edge = new P.StepClock();
  let line = `clockedge ${edge.Advance(0.0, 0.1, 10)} ${edge.Advance(5.0, 0.0, 10)} ${edge.Advance(5.0, -1.0, 10)}`;
  line += ` ${edge.Advance(1.0e6, 0.2, P.kMaxRoundsPerFrame)} ${f64(edge.Fraction())}`;
  line += ` ${edge.Advance(1.0e6, 0.2, P.kMaxRoundsPerFrame)} ${f64(edge.Fraction())}`;
  print(line);
}

//---------------------------------------------------------------------------
// The walk.
//---------------------------------------------------------------------------
for (let r = 0; r < P.kRuleCount; r += 1) {
  for (const count of [1, 2, 3, 4, 5, 8]) {
    for (const [cols, rows] of [[157, 93], [16, 9], [213, 120]]) {
      for (let ground = 0; ground < 3; ground += 1) {
        const w = new P.World();
        w.Reset(cols, rows);
        w.SetRule(P.kRules[r].turns);
        w.PlaceAnts(count);
        if (ground > 0) w.Soup(P.ThresholdU32(ground === 1 ? 0.2 : 0.9), P.SeedSalt(F(r + 1), count));
        const label = `walk r${r} a${count} ${cols}x${rows} g${ground}`;
        state(`${label} seed`, w);
        walk(w, label, [1, 9, 90, 900, 2000], false);
        walk(w, label, [2000, 900, 90, 9, 1], true);
      }
    }
  }
}

{
  const w = new P.World();
  w.Reset(512, 512);
  w.SetRule('RL');
  w.PlaceAnts(1);
  walk(w, 'highway', [1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 977, 1023, 1000, 1000], false);
  walk(w, 'highway', [6000, 6000], true);
}

for (let r = 0; r < P.kRuleCount; r += 1) {
  const w = new P.World();
  w.Reset(31, 17);
  w.SetRule(P.kRules[r].turns);
  w.SetAnts([{ x: 0, y: 0, heading: 3 }, { x: 30, y: 0, heading: 1 }, { x: 0, y: 16, heading: 0 },
    { x: 30, y: 16, heading: 2 }, { x: 15, y: 0, heading: 2 }, { x: 0, y: 8, heading: 3 }]);
  w.Soup(P.ThresholdU32(0.5), (0xc0ffee + r) >>> 0);
  const label = `edges r${r}`;
  state(`${label} seed`, w);
  walk(w, label, [1, 1, 1, 7, 490, 2000], false);
  walk(w, label, [2000, 490, 7, 1, 1, 1], true);
}

{
  const w = new P.World();
  w.Reset(101, 61);
  w.SetRule(P.kRules[4].turns);
  w.PlaceAnts(3);
  w.Soup(P.ThresholdU32(0.6), 77);
  state('rules seed', w);
  walk(w, 'rules LLRRRLRLRLLR', [1500], false);
  print(`setrule LLRR ${w.SetRule('LLRR') ? 1 : 0}`);
  walk(w, 'rules LLRR', [1500], false);
  print(`setrule RLR ${w.SetRule('RLR') ? 1 : 0}`);
  walk(w, 'rules RLR', [700], true);
  print(`setrule RRLLLRLLLRRR ${w.SetRule('RRLLLRLLLRRR') ? 1 : 0}`);
  walk(w, 'rules RRLLLRLLLRRR', [1000], false);
  print(`setrule bogus ${w.SetRule('bogus') ? 1 : 0}`);
  walk(w, 'rules still RRLLLRLLLRRR', [10], false);
}

{
  const w = new P.World();
  w.Reset(213, 120);
  w.SetRule('RL');
  w.PlaceAnts(4);
  w.Soup(P.ThresholdU32(0.3), 5);
  walk(w, 'regrid', [2000], false);
  w.Regrid(160, 120);
  state('regrid 160x120', w);
  walk(w, 'regrid a', [500], false);
  w.Regrid(213, 120);
  state('regrid 213x120', w);
  w.Regrid(100, 57);
  state('regrid 100x57', w);
  walk(w, 'regrid b', [1000], true);
  w.Regrid(300, 200);
  state('regrid 300x200', w);
  w.Regrid(300, 200);
  state('regrid same', w);
  w.Regrid(0, -5);
  state('regrid 1x1', w);
  walk(w, 'regrid c', [10], false);
}

{
  const w = new P.World();
  w.Reset(37, 23);
  w.SetRule(P.kRules[3].turns);
  w.PlaceAnts(2);
  for (const length of [37 * 23, 100, 37 * 23 + 50]) {
    const colours = new Uint8Array(length);
    for (let i = 0; i < length; i += 1) colours[i] = (i * 37 + length) & 255;
    w.Load(colours);
    const label = `load ${length}`;
    state(label, w);
    walk(w, label, [1000], false);
  }
}

{
  const w = new P.World();
  w.Step(10, false);
  state('fresh', w);
  w.Reset(9, 7);
  w.Step(10, false);
  state('noants', w);
  w.PlaceAnts(0);
  w.Step(0, false);
  state('zero', w);
  w.PlaceAnts(99);
  state('clamped', w);
}

{
  const sizes = [];
  for (let n = 1; n <= 40; n += 1) sizes.push([n, n]);
  for (let n = 42; n <= 1080; n += 6) {
    sizes.push([n, n]);
    sizes.push([n + 1, n + 5]);
  }
  sizes.push([1920, 1080], [1080, 1920], [7, 3], [3, 7], [213, 120], [157, 93]);
  for (const [cols, rows] of sizes) {
    const w = new P.World();
    w.Reset(cols, rows);
    for (let count = 1; count <= P.kMaxAnts; count += 1) {
      w.PlaceAnts(count);
      print(`place ${cols}x${rows} ${count} ${ants(w)}`);
    }
  }
}

process.stdout.write(`${out.join('\n')}\n`);
