/**
 * Langton — browser demo.
 *
 * Langton's ant, and Turk and Propp's many-coloured ants. The one idea, from
 * `AGENTS.md`: **an ant on a grid turns right on a white cell and left on a
 * black one, flips the cell and steps forward.** Nothing is drawn as a picture
 * of a walk: the walk is the rule, run exactly, one step at a time, and ten
 * thousand steps of chaos and then the highway fall out of it. SW Langton walks
 * an empty grid or a soup; SW Langton Over walks the clip's brightness.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the four
 *   `R"( ... )"` pieces of `source/Shaders.cpp` plus kVersion, spliced across
 *   by `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character, and checks that they are joined as
 *   `Assemble()` and InitGL join them, and `tools/verify.sh` runs it. They run
 *   as the same two passes -- the Over's sample pass into an R32UI target the
 *   grid's size, read back for the walk, and the composite from the RG32UI
 *   state texture -- with the same uniforms as `LangtonPlugin::seed` and
 *   `composite`.
 *
 *   **The CPU half is a port.** The plugin walks on the CPU (`ant::World`,
 *   Ant.cpp) and uploads the rows it touched; here that walk is JavaScript,
 *   between the PORT markers below, with Hash.h's PCG, Clock.h's step clock
 *   and Controls.cpp's conversions. That block is held to the C++ by a
 *   one-file printer compiled on the plugin's own Ant.cpp and Controls.cpp
 *   (demo/tools/crosscheck/, which tools/verify.sh runs): every rule, 1 to 8
 *   ants, soups and empty grids, thousands of rounds forward and back, and
 *   every cell's colour, visit flag and visit time and every ant come out
 *   identical. The frame logic around it -- `ProcessOpenGL`, `seed()`,
 *   `upload()` and `composite()` from Langton.cpp, below the markers -- is a
 *   port that was compared with the plugin ONCE, frame for frame, through
 *   `lntest --pipe` (2026-10-20, see AGENTS.md: every checkpoint within one
 *   level of 255, the source through Reverse, Step, Rule, Ants, Reseed, Seed
 *   and a re-grid, the Over through its clip seeding and a Reseed). Nothing
 *   repeats that one; a change to Langton.cpp needs this changed by hand.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Both plugins, one page.** `SW Langton` (LA01) is a source and `SW Langton
 * Over` (LA02) an effect: one class with a flag, each declaring only its own
 * controls (`HostOrder()`). The Over adds Clip Colour, Backdrop and Mix, and
 * its Pattern has a third element, Clip, which is its default. The kit's option
 * rows have one fixed element list, so Pattern is two rows of the same name --
 * two elements for the source, three for the Over -- and only the one the
 * chosen plugin declares is shown (conway's shape). The Plugin dropdown is a
 * new instance, as it would be in Resolume, at that constructor's defaults
 * (one ant at 600 rounds a second on an empty grid; four at 3000 on the clip).
 * `?plugin=over` opens the effect; Copy link carries it.
 *
 * **Nothing audio.** `Audio` is an FFT buffer Resolume fills; `Audio Burst`
 * and `Audio Reverse` act on its onsets. A browser has no Resolume FFT, so all
 * three are absent from the panel rather than present and dead. The removal
 * is exact: with no spectrum the analyser never fires, so no burst is added
 * and time is never turned round -- here as in a layer with no audio.
 *
 * **Integers and events.** Ants and Seed are FF_TYPE_INTEGER in the plugin,
 * which the kit has no control for: Ants is a dropdown of all eight values,
 * 1-8 (its slot plus one is the value the host would hold), Seed one of 0-99
 * (the plugin takes 0-9999; the default, 1, is among them). Step and Reseed are
 * FF_TYPE_EVENT; they are toggles the page reads on the rising edge, as the
 * plugin does, then puts back to 0, which is what a host does with an event.
 *
 * **The clock.** The kit's clock is seconds accumulated from frame deltas,
 * capped at 0.1 s a frame, paused by Pause, handed to the port as the host's
 * time already in seconds: the plugin's seconds-or-milliseconds vote is not
 * ported, because this host's unit is known. The jump rule is the plugin's: a
 * step backwards or over a second is a jump and no time passes across it, so
 * Restart leaves the walk where it was.
 *
 * **The Over's clip is read back as RGBA_INTEGER where it must be.** The
 * plugin reads its R32UI sample target with GL_RED_INTEGER. WebGL2 guarantees
 * only RGBA_INTEGER for an unsigned-integer target; the page asks for the
 * implementation's preferred format and reads RED_INTEGER when it is offered,
 * otherwise RGBA_INTEGER and takes the red channel. Same texels, same values.
 *
 * **The harness's test hooks are not ported** (`SetWrapSkewForTest`,
 * `SetUnturnSignForTest`, the point-sample, mirror, float-clock, clear-on-
 * resize and dropped-row controls): they exist so `lntest --negative` has wrong
 * models to catch, and the plugin never sets them in a host. Their uniforms are
 * set to the values the plugin sends in operation (0).
 *
 * **The About block is not a parameter here**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';

// ==== BEGIN PORT ===========================================================
// Hash.h, Ant.h/Ant.cpp, Clock.h and Controls.h/Controls.cpp, ported. No DOM
// and no GL between the two markers: demo/tools/crosscheck/ slices this block
// out by its markers, runs it under node and diffs what it prints against the
// same printout from the plugin's own C++.

const f32 = Math.fround;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

/** std::lround: half away from zero (Math.round alone rounds -2.5 to -2). */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

//---------------------------------------------------------------------------
// Hash.h: PCG in uint32 throughout (= the GLSL's pcg()).
//---------------------------------------------------------------------------

function Pcg(v) {
  const state = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

function CellHash(x, y, salt) {
  return Pcg(((x >>> 0) ^ Pcg(((y >>> 0) ^ Pcg(salt)) >>> 0)) >>> 0);
}

/** LangtonPlugin::seedSalt (Langton.cpp), as a function of the two things it reads. */
function SeedSalt(seedParam, seedSerial) {
  const seedValue = clamp(lround(seedParam), 0, 9999) >>> 0;
  return Pcg((Math.imul(seedValue, 2654435761) ^ Pcg((seedSerial + 0x9e3779b9) >>> 0)) >>> 0);
}

//---------------------------------------------------------------------------
// Ant.h / Ant.cpp: the walk. Rows count UP (row 0 is the bottom, GL's order);
// headings are clockwise on screen: 0 up, 1 right, 2 down, 3 left.
//---------------------------------------------------------------------------

const kMaxColours = 12;
const kMaxAnts = 8;

/** The Rule dropdown, in order: Langton's first, then Turk and Propp's. */
const kRules = [
  { name: 'Langton (RL)', turns: 'RL' },
  { name: 'LLRR', turns: 'LLRR' },
  { name: 'RLLR', turns: 'RLLR' },
  { name: 'LRRRRRLLR', turns: 'LRRRRRLLR' },
  { name: 'LLRRRLRLRLLR', turns: 'LLRRRLRLRLLR' },
  { name: 'RRLLLRLLLRRR', turns: 'RRLLLRLLLRRR' },
  { name: 'RLR', turns: 'RLR' },
];
const kRuleCount = kRules.length;

const kDx = [0, 1, 0, -1];
const kDy = [1, 0, -1, 0];

const kPi = 3.14159265358979323846;

function positiveMod(a, b) {
  const m = a % b;
  return m < 0 ? m + b : m;
}

/** A rule as turns: +1 for R, -1 for L. Empty if it does not parse. */
function ParseTurns(text) {
  const out = [];
  for (const c of text) {
    if (c === 'R' || c === 'r') out.push(1);
    else if (c === 'L' || c === 'l') out.push(-1);
    else return [];
  }
  if (out.length < 2 || out.length > kMaxColours) return [];
  return out;
}

class World {
  constructor() {
    this.cols = 0;
    this.rows = 0;
    this.turns = [1, -1];
    this.colour = new Uint8Array(0);
    this.visited = new Uint8Array(0);
    /** The activity clock's low 32 bits at each cell's last visit. */
    this.visit = new Uint32Array(0);
    /** Each { x, y, heading }. */
    this.ants = [];
    /** Net rounds: + forward, - back. */
    this.rounds = 0;
    /**
     * Every ant step, either way: a uint64 in the plugin, a double here,
     * which is exact to 2^53 steps (285 years at a million rounds a second
     * with eight ants).
     */
    this.clock = 0;
    this.dirtyFirst = 1 << 30;
    this.dirtyLast = -1;
  }

  /** An empty grid (every cell colour 0, never visited), no change to the ants. */
  Reset(newCols, newRows) {
    this.cols = Math.max(newCols, 1);
    this.rows = Math.max(newRows, 1);
    const n = this.cols * this.rows;
    this.colour = new Uint8Array(n);
    this.visited = new Uint8Array(n);
    this.visit = new Uint32Array(n);
    this.rounds = 0;
    this.MarkAllDirty();
  }

  SetRule(text) {
    const parsed = ParseTurns(text);
    if (parsed.length === 0) return false;
    this.turns = parsed;
    return true;
  }

  /** `count` ants at their start places: one in the middle facing up, the rest round it. */
  PlaceAnts(count) {
    count = clamp(count, 1, kMaxAnts);
    this.ants = [];
    const radius = Math.max(2, Math.trunc(Math.min(this.cols, this.rows) / 6));
    for (let i = 0; i < count; i += 1) {
      const a = { x: Math.trunc(this.cols / 2), y: Math.trunc(this.rows / 2), heading: i % 4 };
      if (i > 0) {
        // The rest round the middle, evenly, the first of them to the right.
        const angle = 2.0 * kPi * (i - 1) / (count - 1);
        a.x += lround(radius * Math.cos(angle));
        a.y += lround(radius * Math.sin(angle));
      }
      a.x = positiveMod(a.x, this.cols);
      a.y = positiveMod(a.y, this.rows);
      this.ants.push(a);
    }
  }

  /** Exactly these ants (the harness's; the cross-check uses it for edge starts). */
  SetAnts(list) {
    this.ants = list.map((a) => ({ x: a.x, y: a.y, heading: a.heading }));
  }

  /** Each cell a random colour 1..n-1 with probability densityU / 2^32, else 0. */
  Soup(densityU, salt) {
    const n = this.turns.length;
    for (let y = 0; y < this.rows; y += 1) {
      for (let x = 0; x < this.cols; x += 1) {
        const h = CellHash(x, y, salt);
        this.colour[y * this.cols + x] = h < densityU ? 1 + (Pcg(h) % (n - 1)) : 0;
      }
    }
    this.MarkAllDirty();
  }

  /** The grid from outside (the Over's clip), colour per cell, row 0 at the bottom. */
  Load(colours) {
    const n = this.turns.length;
    const count = Math.min(colours.length, this.colour.length);
    for (let i = 0; i < count; i += 1) this.colour[i] = colours[i] % n;
    this.MarkAllDirty();
  }

  wrap(a) {
    // At most one cell outside: one add or subtract per axis.
    if (a.x < 0) a.x += this.cols;
    else if (a.x >= this.cols) a.x -= this.cols;
    if (a.y < 0) a.y += this.rows;
    else if (a.y >= this.rows) a.y -= this.rows;
  }

  dirty(y) {
    this.dirtyFirst = Math.min(this.dirtyFirst, y);
    this.dirtyLast = Math.max(this.dirtyLast, y);
  }

  forward(a) {
    const n = this.turns.length;
    const i = a.y * this.cols + a.x;
    const c = this.colour[i] % n;
    a.heading = (a.heading + this.turns[c] + 4) & 3;
    this.colour[i] = (c + 1) % n;
    this.visited[i] = 1;
    this.visit[i] = this.clock >>> 0;
    this.dirty(a.y);
    a.x += kDx[a.heading];
    a.y += kDy[a.heading];
    this.wrap(a);
    this.clock += 1;
  }

  backward(a) {
    const n = this.turns.length;
    a.x -= kDx[a.heading];
    a.y -= kDy[a.heading];
    this.wrap(a);
    const i = a.y * this.cols + a.x;
    const c = ((this.colour[i] % n) + n - 1) % n;
    this.colour[i] = c;
    this.visited[i] = 1;
    this.visit[i] = this.clock >>> 0;
    this.dirty(a.y);
    a.heading = (a.heading - this.turns[c] + 4) & 3;
    this.clock += 1;
  }

  /** `count` rounds (every ant one step each), or back if `reverse`, ants in reverse order. */
  Step(count, reverse) {
    if (this.ants.length === 0 || this.cols <= 0 || this.rows <= 0) return;
    const ants = this.ants;
    for (let r = 0; r < count; r += 1) {
      if (!reverse) {
        for (let k = 0; k < ants.length; k += 1) this.forward(ants[k]);
        this.rounds += 1;
      } else {
        for (let k = ants.length - 1; k >= 0; k -= 1) this.backward(ants[k]);
        this.rounds -= 1;
      }
    }
  }

  /** A new size: the overlap kept, centred (offsets TRUNCATED, not floored); ants wrap. */
  Regrid(newCols, newRows) {
    newCols = Math.max(newCols, 1);
    newRows = Math.max(newRows, 1);
    if (newCols === this.cols && newRows === this.rows) return;
    const ox = Math.trunc((this.cols - newCols) / 2);
    const oy = Math.trunc((this.rows - newRows) / 2);
    const n = newCols * newRows;
    const c2 = new Uint8Array(n);
    const v2 = new Uint8Array(n);
    const t2 = new Uint32Array(n);
    for (let y = 0; y < newRows; y += 1) {
      for (let x = 0; x < newCols; x += 1) {
        const sx = x + ox;
        const sy = y + oy;
        if (sx < 0 || sy < 0 || sx >= this.cols || sy >= this.rows) continue;
        const from = sy * this.cols + sx;
        const to = y * newCols + x;
        c2[to] = this.colour[from];
        v2[to] = this.visited[from];
        t2[to] = this.visit[from];
      }
    }
    this.colour = c2;
    this.visited = v2;
    this.visit = t2;
    for (const a of this.ants) {
      a.x = positiveMod(a.x - ox, newCols);
      a.y = positiveMod(a.y - oy, newRows);
    }
    this.cols = newCols;
    this.rows = newRows;
    this.MarkAllDirty();
  }

  Colours() { return this.turns.length; }

  /** The rows touched since the last call (inclusive), or null if none. */
  TakeDirtyRows() {
    if (this.dirtyLast < this.dirtyFirst) return null;
    const first = Math.max(this.dirtyFirst, 0);
    const last = Math.min(this.dirtyLast, this.rows - 1);
    this.dirtyFirst = 1 << 30;
    this.dirtyLast = -1;
    return last >= first ? { first, last } : null;
  }

  MarkAllDirty() {
    this.dirtyFirst = 0;
    this.dirtyLast = this.rows - 1;
  }
}

//---------------------------------------------------------------------------
// Clock.h: owed rounds in double, whole ones taken, at most maxSteps, the
// rest dropped.
//---------------------------------------------------------------------------

class StepClock {
  constructor() { this.owed = 0.0; }

  Advance(stepsPerSecond, dt, maxSteps) {
    if (stepsPerSecond <= 0.0 || dt <= 0.0) return 0;
    this.owed += stepsPerSecond * dt;
    let whole = Math.floor(this.owed);
    if (whole > maxSteps) {
      whole = maxSteps;
      this.owed = Math.min(this.owed - whole, 0.999999);
    } else {
      this.owed -= whole;
    }
    return whole;
  }

  Fraction() { return this.owed; }

  Reset() { this.owed = 0.0; }
}

//---------------------------------------------------------------------------
// Controls.h / Controls.cpp: every host 0..1 to the plugin's units. A value
// arrives as the float the host holds (f32), as the C++ signatures take it.
//---------------------------------------------------------------------------

const kMaxRows = 1080;
const kMinRows = 16;
const kMaxRoundsPerFrame = 100000;

const clamp01 = (v) => clamp(v, 0.0, 1.0);
const geometric = (v, low, high) => low * Math.pow(high / low, clamp01(v));
function inverseGeometric(value, low, high) {
  const lo = Math.min(low, high);
  const hi = Math.max(low, high);
  return f32(Math.log(clamp(value, lo, hi) / low) / Math.log(high / low));
}
const kSpeedLow = 1.0;
const kSpeedHigh = 1.0e6;
const kTrailLow = 10.0;
const kTrailHigh = 1.0e6;

/** Rounds per second: exactly 0 at the bottom, then 1 to 1,000,000, geometrically. */
const SpeedFromParam = (v) => (v <= 0.0 ? 0.0 : geometric(v, kSpeedLow, kSpeedHigh));
const ParamFromSpeed = (rps) => (rps <= 0.0 ? 0.0 : inverseGeometric(rps, kSpeedLow, kSpeedHigh));
const DensityFromParam = (v) => clamp01(v);
const RowsFromParam = (v) => lround(geometric(v, kMaxRows, kMinRows));
const ParamFromRows = (rows) => inverseGeometric(rows, kMaxRows, kMinRows);
const GapFromParam = (v) => 0.5 * clamp01(v);
const TrailFromParam = (v) => (v <= 0.0 ? 0.0 : geometric(v, kTrailLow, kTrailHigh));
const ParamFromTrail = (steps) => (steps <= 0.0 ? 0.0 : inverseGeometric(steps, kTrailLow, kTrailHigh));

/** A probability as the integer threshold a 32-bit hash is compared with. */
function ThresholdU32(probability) {
  if (!(probability > 0.0)) return 0;
  const scaled = Math.round(probability * 4294967296.0); // std::round; positive here
  return scaled >= 4294967295.0 ? 0xffffffff : scaled >>> 0;
}

/** The grid for a raster: `rows` rows, columns from the raster's aspect. */
function GridFor(rows, width, height) {
  const r = Math.max(rows, 1);
  const aspect = height > 0 ? width / height : 1.0;
  return { cols: Math.max(1, lround(r * aspect)), rows: r };
}

const OptionIndex = (value, count) => clamp(lround(value), 0, count - 1);

/** What Reseed lays down; the Over adds Clip, LAST. */
const PATTERN_NAMES = ['Empty', 'Soup', 'Clip'];
const kPatternEmpty = 0;
const kPatternSoup = 1;
const kPatternClip = 2;
const PatternCount = (effect) => (effect ? 3 : 2);
const PALETTE_NAMES = ['Phosphor', 'Heat', 'Ice', 'Mono', 'Spectrum'];
const kPaletteSpectrum = 4;

// ==== END PORT =============================================================

//===========================================================================
// Langton.cpp's lookOf() and hueOf(): the palettes, in linear light.
//===========================================================================

const LOOKS = [
  { first: [0.80, 1.00, 0.80], last: [0.05, 0.50, 0.15], trail: [0.20, 0.70, 0.30], ant: [1.00, 0.30, 0.15] }, // Phosphor
  { first: [1.00, 0.95, 0.70], last: [0.80, 0.12, 0.02], trail: [0.70, 0.25, 0.05], ant: [0.35, 0.80, 1.00] }, // Heat
  { first: [0.92, 0.98, 1.00], last: [0.10, 0.35, 0.95], trail: [0.20, 0.45, 0.90], ant: [1.00, 0.60, 0.15] }, // Ice
  { first: [1.00, 1.00, 1.00], last: [1.00, 1.00, 1.00], trail: [0.50, 0.50, 0.50], ant: [1.00, 0.20, 0.20] }, // Mono
  { first: [1.00, 1.00, 1.00], last: [1.00, 1.00, 1.00], trail: [0.35, 0.35, 0.40], ant: [1.00, 1.00, 1.00] }, // Spectrum
].map((look) => Object.fromEntries(Object.entries(look).map(([k, v]) => [k, v.map(f32)])));

/** A hue wheel from clamped triangles (no trig), in double, stored as float. */
function hueOf(h, out, at) {
  const offsets = [0.0, 2.0 / 3.0, 1.0 / 3.0];
  for (let i = 0; i < 3; i += 1) {
    let f = h + offsets[i];
    f -= Math.floor(f);
    out[at + i] = f32(clamp(Math.abs(f * 6.0 - 3.0) - 1.0, 0.0, 1.0));
  }
}

//===========================================================================
// The plugin's per-instance state and ProcessOpenGL, ported.
//===========================================================================

/** Host seconds: a bigger forward step, or any backward one, is a jump. */
const kMaxFrameDelta = 1.0;

function blankTexture(gl) {
  const t = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, t);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return t;
}

/** One instance: what `LangtonPlugin` holds between frames. */
class LangtonState {
  constructor(gl, isEffect) {
    this.gl = gl;
    this.isEffect = isEffect;

    // PassBuffer::Ensure's formats and sampling (NEAREST: an integer texture
    // cannot be filtered). WebGL2 zero-initialises a new texture, which is the
    // plugin's Clear() on allocation.
    this.state = new PassBuffer(gl, { filter: 'nearest' });
    this.sampleTarget = new PassBuffer(gl, { filter: 'nearest' });
    this.blankClip = blankTexture(gl);
    this.staging = new Uint32Array(0);
    this.sampleRead = null;

    this.world = new World();
    this.grid = { cols: 0, rows: 0 };

    // Time.
    this.lastNow = -1.0;
    this.clock = new StepClock();

    this.needSeed = true;
    this.seedSerial = 0;
    this.lastPattern = -1;
    this.lastSeed = -1;
    this.lastRule = -1;
    this.lastAnts = -1;
    this.stepHeld = false;
    this.reseedHeld = false;
    this.pendingRounds = 0;
    // Only an audio onset with Audio Reverse flips this, and there is no audio.
    this.audioReversed = false;
    this.reversedNow = false;
    this.reseeds = 0;
    this.stats = null;
  }

  dispose() {
    this.state.dispose();
    this.sampleTarget.dispose();
    this.gl.deleteTexture(this.blankClip);
  }
}

function createRenderer(gl, quad) {
  // InitGL: the vertex stage is kVersion + kQuadVertex; each fragment stage is
  // Assemble( piece ) = kVersion + kCommon + piece.
  const vertex = S.VERSION + S.QUAD_VERTEX;
  const assemble = (piece) => S.VERSION + S.COMMON + piece;
  const programs = {
    sample: new Program(gl, vertex, assemble(S.SAMPLE_FRAGMENT), 'sample'),
    composite: new Program(gl, vertex, assemble(S.COMPOSITE_FRAGMENT), 'composite'),
  };

  let instance = null;
  let instanceVariant = null;

  /** glUniform2i, which the kit's Program has no sugar for. */
  const setIvec2 = (program, name, x, y) => {
    const loc = program.location(name);
    if (loc === null) program.missing.add(name);
    else gl.uniform2i(loc, x | 0, y | 0);
  };

  const unbind = (count) => {
    for (let unit = count - 1; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
    gl.activeTexture(gl.TEXTURE0);
  };

  /**
   * The Over's sample pass and its read-back: the clip's brightness at each
   * cell's centre, in the rule's levels, one uint per cell, row 0 at the bottom.
   */
  function sampleClip(st, clip, width, height) {
    const { cols, rows } = st.grid;
    const target = st.sampleTarget;
    gl.bindFramebuffer(gl.FRAMEBUFFER, target.fbo);
    gl.viewport(0, 0, target.width, target.height);
    const p = programs.sample.use();
    bindTexture(gl, 0, clip);
    p.setSampler('Clip', 0);
    setIvec2(p, 'ClipRaster', width, height);
    setIvec2(p, 'Grid', cols, rows);
    p.setInt('Colours', st.world.Colours());
    p.setInt('MirrorForTest', 0);
    quad.draw();
    unbind(1);

    // GL_RED_INTEGER, as the plugin reads it, where the implementation offers
    // it; otherwise RGBA_INTEGER, which WebGL2 always accepts from a uint target.
    const cells = cols * rows;
    if (!st.sampleRead || st.sampleRead.cells !== cells) {
      const format = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_FORMAT);
      const type = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_TYPE);
      const red = format === gl.RED_INTEGER && type === gl.UNSIGNED_INT;
      st.sampleRead = { cells, red, data: new Uint32Array(cells * (red ? 1 : 4)) };
    }
    const read = st.sampleRead;
    const alignment = gl.getParameter(gl.PACK_ALIGNMENT);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
    gl.readPixels(0, 0, cols, rows, read.red ? gl.RED_INTEGER : gl.RGBA_INTEGER, gl.UNSIGNED_INT, read.data);
    gl.pixelStorei(gl.PACK_ALIGNMENT, alignment);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    const stride = read.red ? 1 : 4;
    const colours = new Uint8Array(cells);
    for (let i = 0; i < cells; i += 1) colours[i] = read.data[i * stride] & 0xff;
    return colours;
  }

  /** LangtonPlugin::seed: a fresh field -- the rule, the ants at their start places, the ground. */
  function seed(st, host, clip, width, height) {
    const ruleIndex = OptionIndex(host.rule, kRuleCount);
    st.world.Reset(st.grid.cols, st.grid.rows);
    st.world.SetRule(kRules[ruleIndex].turns);
    st.world.PlaceAnts(clamp(lround(host.ants), 1, kMaxAnts));
    const pattern = OptionIndex(host.pattern, PatternCount(st.isEffect));
    if (pattern === kPatternSoup) {
      st.world.Soup(ThresholdU32(DensityFromParam(host.density)), SeedSalt(host.seed, st.seedSerial));
    } else if (pattern === kPatternClip && clip) {
      // The clip, a level per cell, read back for the walk: once a reseed.
      st.world.Load(sampleClip(st, clip, width, height));
    }
    st.lastRule = ruleIndex;
    st.lastAnts = st.world.ants.length;
    st.reseeds += 1;
  }

  /** LangtonPlugin::upload: the rows the walk touched, into the RG32UI state. */
  function upload(st) {
    const range = st.world.TakeDirtyRows();
    if (!range) return;
    const { first, last } = range;
    const count = last - first + 1;
    const cols = st.world.cols;
    const need = count * cols * 2;
    if (st.staging.length < need) st.staging = new Uint32Array(need);
    const staging = st.staging.subarray(0, need);
    const { colour, visited, visit } = st.world;
    for (let y = 0; y < count; y += 1) {
      for (let x = 0; x < cols; x += 1) {
        const from = (first + y) * cols + x;
        const to = (y * cols + x) * 2;
        staging[to] = colour[from] | (visited[from] ? 0x100 : 0);
        staging[to + 1] = visit[from];
      }
    }
    // PassBuffer::UploadRows.
    const target = st.state;
    if (!target.texture || count <= 0 || first < 0 || first + count > target.height) return;
    const alignment = gl.getParameter(gl.UNPACK_ALIGNMENT);
    gl.bindTexture(gl.TEXTURE_2D, target.texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, first, target.width, count, gl.RG_INTEGER, gl.UNSIGNED_INT, staging);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, alignment);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  /** LangtonPlugin::composite, into the canvas: the viewport is the host's. */
  function composite(st, host, clip, raster, viewWidth, viewHeight) {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, viewWidth, viewHeight);
    const palette = OptionIndex(host.palette, PALETTE_NAMES.length);
    const look = LOOKS[palette];

    // The rule's colours: 1 is `first`, n - 1 is `last` (Spectrum: the hue
    // wheel), 0 is never drawn.
    const colours = new Float32Array(kMaxColours * 3);
    const n = Math.max(st.world.Colours(), 2);
    for (let k = 1; k < n; k += 1) {
      if (palette === kPaletteSpectrum) {
        hueOf(n > 2 ? (k - 1) / (n - 1) : 0.0, colours, k * 3);
      } else {
        const t = n > 2 ? f32(f32(k - 1) / f32(n - 2)) : 0.0;
        for (let i = 0; i < 3; i += 1) {
          colours[k * 3 + i] = f32(look.first[i] + f32(f32(look.last[i] - look.first[i]) * t));
        }
      }
    }

    const antPlaces = new Int32Array(kMaxAnts * 2);
    const ants = st.world.ants;
    const antCount = host.showAnts > 0.5 ? Math.min(ants.length, kMaxAnts) : 0;
    for (let i = 0; i < antCount; i += 1) {
      antPlaces[i * 2] = ants[i].x;
      antPlaces[i * 2 + 1] = ants[i].y;
    }

    const p = programs.composite.use();
    bindTexture(gl, 0, st.state.texture);
    bindTexture(gl, 1, clip ?? st.blankClip);
    p.setSampler('State', 0);
    p.setSampler('InputTexture', 1);
    setIvec2(p, 'Grid', st.grid.cols, st.grid.rows);
    p.set('Raster', raster[0], raster[1]);
    p.set('ViewOrigin', 0, 0);
    p.set('Gap', GapFromParam(host.gap));
    p.setUint('Now', st.world.clock);
    p.set('Trail', TrailFromParam(host.trail));
    p.setArray('Palette', colours, 3);
    p.set('TrailColour', ...look.trail);
    p.set('AntColour', ...look.ant);
    const antsLoc = p.location('Ants[0]');
    if (antsLoc === null) p.missing.add('Ants');
    else gl.uniform2iv(antsLoc, antPlaces);
    p.setInt('AntCount', antCount);
    p.setInt('IsEffect', st.isEffect ? 1 : 0);
    p.set('ClipColour', st.isEffect ? clamp(host.clipColour, 0.0, 1.0) : 0.0);
    p.set('Backdrop', st.isEffect ? clamp(host.backdrop, 0.0, 1.0) : 0.0);
    p.set('MixAmount', st.isEffect ? clamp(host.mix, 0.0, 1.0) : 1.0);
    p.setInt('PointSampleForTest', 0);
    quad.draw();
    unbind(2);
  }

  return {
    get stats() { return instance?.stats ?? null; },
    get programs() { return programs; },
    /** For the headless check: the instance's walk, read-only by convention. */
    get world() { return instance?.world ?? null; },

    render({ input, params, width, height, time, variant }) {
      const isEffect = variant === 'over';
      if (!instance || instanceVariant !== variant) {
        instance?.dispose();
        instance = new LangtonState(gl, isEffect);
        instanceVariant = variant;
      }
      const st = instance;

      // The plugin's params[]: what the host set, as a float. Ants is held as
      // its dropdown slot here, so the host's integer is the slot plus one.
      const v = (id) => f32(params.get(id));
      const host = {
        rule: v('rule'),
        ants: f32(params.get('ants') + 1),
        speed: v('speed'),
        reverse: v('reverse'),
        step: v('step'),
        pattern: v(isEffect ? 'patternOver' : 'pattern'),
        density: v('density'),
        seed: v('seed'),
        reseed: v('reseed'),
        cellSize: v('cellSize'),
        gap: v('gap'),
        palette: v('palette'),
        trail: v('trail'),
        showAnts: v('showAnts'),
        clipColour: v('clipColour'),
        backdrop: v('backdrop'),
        mix: v('mix'),
      };
      gl.disable(gl.BLEND);

      // The raster: the clip's for the Over, the host viewport's for the source.
      const w = isEffect ? input.width : width;
      const h = isEffect ? input.height : height;
      const clip = isEffect ? input.texture : null;

      //-------------------------------------------------------------------
      // Time: the host's (the page's clock, in seconds), frame to frame. A
      // backwards or large step is a jump and no time passes across it (a
      // jump also resets the audio analyser; there is none here).
      //-------------------------------------------------------------------
      let dt = 0.0;
      if (st.lastNow >= 0.0) {
        const stepTime = time - st.lastNow;
        if (!(stepTime < 0.0 || stepTime > kMaxFrameDelta)) dt = stepTime;
      }
      st.lastNow = time;

      //-------------------------------------------------------------------
      // Audio: nothing. With no spectrum the analyser never fires, so no
      // burst is added and time is never turned round.
      //-------------------------------------------------------------------

      //-------------------------------------------------------------------
      // The buttons (a press is the rising edge), and what reseeds. A new
      // Seed starts its sequence again before a Reseed press is read.
      //-------------------------------------------------------------------
      const stepDown = host.step > 0.5;
      if (stepDown && !st.stepHeld) st.pendingRounds += 1;
      st.stepHeld = stepDown;
      const seedValue = clamp(lround(host.seed), 0, 9999);
      if (seedValue !== st.lastSeed) {
        st.seedSerial = 0;
        st.needSeed = true;
      }
      st.lastSeed = seedValue;
      const reseedDown = host.reseed > 0.5;
      if (reseedDown && !st.reseedHeld) {
        st.seedSerial = (st.seedSerial + 1) >>> 0;
        st.needSeed = true;
      }
      st.reseedHeld = reseedDown;
      const pattern = OptionIndex(host.pattern, PatternCount(isEffect));
      if (pattern !== st.lastPattern) st.needSeed = true;
      st.lastPattern = pattern;

      //-------------------------------------------------------------------
      // The grid follows Cell Size and the raster's ASPECT. A new size keeps
      // the overlap and the ants, centred.
      //-------------------------------------------------------------------
      const want = GridFor(RowsFromParam(host.cellSize), w, h);
      if (st.world.cols === 0) st.needSeed = true;
      else if (want.cols !== st.grid.cols || want.rows !== st.grid.rows) st.world.Regrid(want.cols, want.rows);
      st.grid = want;

      //-------------------------------------------------------------------
      // Everything allocated before anything is bound. A new texture is
      // cleared, so all of the walk goes up into it.
      //-------------------------------------------------------------------
      const reallocated = st.state.width !== want.cols || st.state.height !== want.rows;
      st.state.ensure(want.cols, want.rows, gl.RG32UI);
      if (isEffect) st.sampleTarget.ensure(want.cols, want.rows, gl.R32UI);
      if (reallocated) st.world.MarkAllDirty();

      if (st.needSeed) {
        st.needSeed = false;
        seed(st, host, clip, w, h);
        st.world.MarkAllDirty();
      }

      //-------------------------------------------------------------------
      // A Rule change carries the field on under the new rule; an Ants change
      // puts the ants back at their start places on the same field.
      //-------------------------------------------------------------------
      const ruleIndex = OptionIndex(host.rule, kRuleCount);
      if (ruleIndex !== st.lastRule) {
        st.world.SetRule(kRules[ruleIndex].turns);
        st.lastRule = ruleIndex;
      }
      const antCount = clamp(lround(host.ants), 1, kMaxAnts);
      if (antCount !== st.lastAnts) {
        st.world.PlaceAnts(antCount);
        st.lastAnts = antCount;
      }

      //-------------------------------------------------------------------
      // The walk: what the clock owes, then the presses, at most
      // kMaxRoundsPerFrame. Reverse and the audio's turn combine.
      //-------------------------------------------------------------------
      const speed = SpeedFromParam(host.speed);
      const owed = st.clock.Advance(speed, dt, kMaxRoundsPerFrame);
      const rounds = Math.min(owed + st.pendingRounds, kMaxRoundsPerFrame);
      st.pendingRounds = 0;
      st.reversedNow = (host.reverse > 0.5) !== st.audioReversed;
      st.world.Step(rounds, st.reversedNow);

      upload(st);
      composite(st, host, clip, [w, h], width, height);

      // A host lets go of an event button; the kit has none, so the page does.
      if (stepDown) params.set('step', 0);
      if (reseedDown) params.set('reseed', 0);

      st.stats = {
        speed,
        rounds,
        grid: st.grid,
        net: st.world.rounds,
        steps: st.world.clock,
        ants: st.world.ants.length,
        rule: kRules[ruleIndex].turns,
        reversed: st.reversedNow,
        reseeds: st.reseeds,
      };
    },
  };
}

//===========================================================================
// The parameters: each constructor's, in HostOrder()'s order and groups,
// with the plugin's names and defaults. `only` says which plugin declares it.
//===========================================================================

const fmt = (n, digits = 0) => n.toFixed(digits);
const grouped = (n) => Math.round(n).toLocaleString('en-GB');
/** Three significant figures, for the geometric controls. */
const sig = (n) => (n >= 1000 ? grouped(n) : n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n.toFixed(2));

/**
 * The constructor's defaults, source then Over. Each holds every id, because
 * the plugin's params[] does. They differ in Ants (1 / 4; held as the slot,
 * 0 / 3), Speed (600 / 3000 rounds a second) and Pattern (Empty / Clip; here
 * two rows, see the header).
 */
const COMMON_DEFAULTS = {
  rule: 0, ants: 0, speed: ParamFromSpeed(600.0), reverse: 0, step: 0,
  pattern: kPatternEmpty, patternOver: kPatternClip, density: f32(0.2), seed: 1, reseed: 0,
  cellSize: ParamFromRows(120), gap: f32(0.2), palette: 0, trail: ParamFromTrail(3000.0), showAnts: 1,
  clipColour: 0.0, backdrop: f32(0.35), mix: 1.0,
};
const DEFAULTS = {
  source: { ...COMMON_DEFAULTS },
  over: { ...COMMON_DEFAULTS, ants: 3, speed: ParamFromSpeed(3000.0) },
};

const query = new URLSearchParams(window.location.search);
const initialVariant = query.get('plugin') === 'over' ? 'over' : 'source';

const std = (id, name, group, display, hint, only) => ({ id, name, type: 'standard', group, display, hint, only });
const opt = (id, name, elements, group, hint, only) => ({ id, name, type: 'option', elements, group, hint, only });
const bool = (id, name, group, hint, only) => ({ id, name, type: 'boolean', group, hint, only });

const SEED_CHOICES = 100;

const PARAMS = [
  opt('rule', 'Rule', kRules.map((r) => r.name), 'Automaton',
    'The turn for each colour, one letter per colour (R: right, L: left): Langton’s own RL, then Turk and Propp’s many-coloured ants as Wikipedia lists them. Changing it carries the field on under the new rule: colours are read modulo its length.'),
  opt('ants', 'Ants', Array.from({ length: kMaxAnts }, (_, i) => String(i + 1)), 'Automaton',
    'How many ants, taking turns, ant 1 first: the first in the middle facing up, the rest evenly round it, each facing the next heading round. Changing it puts the ants back at their start places on the same field. FF_TYPE_INTEGER 1–8 in the plugin.'),

  std('speed', 'Speed', 'Time', (v) => (SpeedFromParam(f32(v)) === 0 ? 'paused' : `${sig(SpeedFromParam(f32(v)))} rounds/s`),
    'Rounds a second (a round is every ant one step): exactly 0 at the bottom (the ants stand still; Step still works), then 1 to 1,000,000, geometrically. A frame runs at most 100,000 rounds; the rest are dropped, not banked.'),
  bool('reverse', 'Reverse', 'Time',
    'Time runs backwards: every step is undone exactly (step back, colour back, turn back), the ants in the opposite order, so the walk un-happens into where it came from.'),
  bool('step', 'Step', 'Time',
    'One round, by hand (backwards with Reverse). FF_TYPE_EVENT in the plugin; a toggle the page releases after one frame here. (The transport’s Step under the picture is one frame of time, not a round.)'),

  opt('pattern', 'Pattern', PATTERN_NAMES.slice(0, 2), 'Seeding',
    'What the field starts from, and what Reseed lays down. Empty: every cell colour 0, Langton’s own start. Soup: random colours at Density. Choosing one reseeds at once.', 'source'),
  opt('patternOver', 'Pattern', PATTERN_NAMES, 'Seeding',
    'What the field starts from, and what Reseed lays down. Clip (the Over’s default): the clip’s brightness at each cell, cut into as many levels as the rule has colours. Empty and Soup as in the source. Choosing one reseeds at once.', 'over'),
  std('density', 'Density', 'Seeding', (v) => `${fmt(100 * DensityFromParam(f32(v)), 0)}%`,
    'The soup’s fill: the share of cells given a colour other than 0. Acts from the next reseed.'),
  opt('seed', 'Seed', Array.from({ length: SEED_CHOICES }, (_, i) => String(i)), 'Seeding',
    'Which soup: the same Seed always gives the same first soup. Changing it reseeds. FF_TYPE_INTEGER 0–9999 in the plugin; 0–99 here.'),
  bool('reseed', 'Reseed', 'Seeding',
    'The next soup in the Seed’s sequence, the empty grid again, or the clip as it is now; the ants go back to their start places. FF_TYPE_EVENT in the plugin; a toggle the page releases after one frame here.'),

  std('cellSize', 'Cell Size', 'Look', (v) => `${RowsFromParam(f32(v))} rows`,
    'Rows of the grid: 1080 (a cell a pixel tall at 1080p) to 16, geometrically. Columns follow the output’s shape. Changing it keeps the walk, cropped or extended round its middle.'),
  std('gap', 'Gap', 'Look', (v) => `${fmt(GapFromParam(f32(v)), 2)} cell`, 'Dark space round each painted cell, 0 to half a cell.'),
  opt('palette', 'Palette', PALETTE_NAMES, 'Look',
    'The rule’s colours, from colour 1 to its last (colour 0 is never drawn), the trail’s and the ants’. Spectrum spreads the colours round the hue wheel.'),
  std('trail', 'Trail', 'Look', (v) => (TrailFromParam(f32(v)) === 0 ? 'off' : `${sig(TrailFromParam(f32(v)))} steps`),
    'How long a cell glows after an ant has stood on it, in ant steps: off, then 10 to 1,000,000 (its glow falls by e every Trail steps).'),
  bool('showAnts', 'Show Ants', 'Look', 'Each ant’s own cell in the palette’s ant colour.'),
  std('clipColour', 'Clip Colour', 'Look', (v) => fmt(v, 2), 'The painted cells take the clip’s colour under them.', 'over'),
  std('backdrop', 'Backdrop', 'Look', (v) => fmt(v, 2), 'How much of the clip shows behind the cells.', 'over'),
  std('mix', 'Mix', 'Look', (v) => fmt(v, 2), 'The whole effect against the clip. At 0 the clip is returned exactly, alpha and all.', 'over'),
];
for (const p of PARAMS) p.default = DEFAULTS[initialVariant][p.id];

let renderer = null;

const mounted = mountDemo({
  name: 'Langton',
  pluginId: 'LA01 · LA02',
  kind: ['source', 'effect'],
  tagline:
    'Langton’s ant. It turns right on a white cell and left on a black one, flips the cell and steps forward, and from that alone come ten thousand steps of chaos and then a highway, for ever. Nothing is drawn as a picture of a walk: the walk is the rule, run exactly, one step at a time. Twelve-colour ants too, up to eight at once, and time runs backwards: every step can be undone. SW Langton walks an empty grid or a soup; SW Langton Over walks your clip, its brightness the ground the ants read and repaint.',
  repo: 'https://github.com/stoatworks-labs/langton',

  blurb:
    'It is Langton’s own sample and composite passes, ported from the repository to WebGL2, drawing a JavaScript port of the plugin’s CPU walk — the rules, the step and its exact inverse, the torus, the soup, the re-grid, the step clock — which a check in the repository holds to the plugin’s C++ step for step. The frame logic around the walk is a port that was compared with the plugin once, frame for frame. There is no audio here, so the audio controls are absent. SW Langton reads no video; SW Langton Over walks a generated clip.',

  // The source is transparent where nothing is painted; the backdrop shows it.
  showBackdrop: true,

  variants: {
    label: 'Plugin',
    default: initialVariant,
    options: [
      { id: 'source', name: 'SW Langton (source)', hint: 'LA01, FF_SOURCE: ants on an empty grid or a soup. Reads no clip.' },
      { id: 'over', name: 'SW Langton Over (effect)', hint: 'LA02, FF_EFFECT: the clip’s brightness, in the rule’s colours, is the ground the ants walk.' },
    ],
  },

  // For the Over only. The geometry card gives the default two-colour rule
  // clean bright shapes on black to walk and repaint; the ramps and the scene
  // put every level of a many-coloured rule in front of the ants.
  sources: ['grid', 'ramp', 'scene', 'spot', 'bars', 'alpha'],

  params: PARAMS,

  differences: [
    'The walk is a PORT, not the plugin’s code. The plugin walks on the CPU (Ant.cpp) and uploads the rows it touched; here the same walk — the turn table, the step and its exact inverse, the torus, the ants’ start places, the soup from the plugin’s PCG hash, the centred re-grid, the dirty rows — is JavaScript, with the plugin’s step clock and its control conversions. That part was compared with the plugin’s own C++: a one-file program built on Ant.cpp and Controls.cpp and the JavaScript run side by side for all seven rules, 1 to 8 ants, soups and empty grids, ants starting on the edges, thousands of rounds forward and the same back, a rule change mid-walk and re-grids, and printed identically, every cell’s colour, visit and time of visit and every ant. The repository’s verify script repeats that comparison. It found one bit-level difference, which is the compiler’s and not the port’s: the plugin’s Apple Silicon build fuses one multiply-add in its step clock, so the fraction of a round it carries can differ from this page’s in its last bits; the plugin’s Intel build agrees with this page exactly, and no frame’s count of rounds differed. The frame logic around it (the time from frame to frame, the buttons, reseeding, Rule and Ants changes, Reverse, the re-grid, the clip seeding, the upload) is a hand translation of Langton.cpp. It was compared ONCE with the plugin, frame for frame, through its harness on the same synthetic clock: 700 frames of the source through a soup, Reverse, Speed 0 and Step presses, Rule and Ants changes, Reseed, a Seed change and a re-grid, and 240 frames of the Over seeded from this page’s own clip and reseeded, every checkpoint within one level of 255 of the plugin’s picture. Nothing repeats that comparison.',
    'The GPU half is not a port: the sample and composite passes are the plugin’s own GLSL, on the same RG32UI state texture (texelFetch and integer arithmetic), and demo/tools/check_shaders.py fails the repository’s verify script if a character of them, or the way they are joined, drifts.',
    'Nothing audio. The plugin declares an Audio FFT buffer that Resolume fills, with Audio Burst (rounds per onset) and Audio Reverse (each onset turns time round) over it. A browser has no Resolume FFT, so all three are absent from this panel rather than present and dead. With no spectrum the plugin’s analyser never fires, so leaving them out changes nothing the page draws.',
    'Step and Reseed are FF_TYPE_EVENT in the plugin. The kit has no event control, so they are toggles the page releases after one frame — one press, one rising edge, which is what the plugin counts. Ants (FF_TYPE_INTEGER 1–8) is a dropdown of all eight; Seed (FF_TYPE_INTEGER 0–9999) is a dropdown offering 0–99. The About block is not on the panel.',
    'Switching the Plugin dropdown is a new instance, as it would be in Resolume: the walk starts again and every control goes to that plugin’s constructor default (the source: one ant, 600 rounds a second, an empty grid; the Over: four ants, 3000 a second, the clip). The Over declares three more controls (Clip Colour, Backdrop, Mix) and a third Pattern, Clip, its default; Pattern is shown as two rows of the same name because the kit’s dropdowns have one fixed list, and only the chosen plugin’s is visible.',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second a frame. The plugin’s vote on whether the host sends seconds or milliseconds is not ported, because this host’s unit is known. Restart is a clock jump, which the plugin takes as no time passing, so the walk carries on; switching the Plugin dropdown is what starts it again. Paused, the Step control still runs one round per press.',
    'The walk runs on this page’s main thread, as the plugin runs it on Resolume’s render thread, at most 100,000 rounds a frame. At the top of Speed with eight ants a browser may draw fewer frames a second than Resolume would, and then the ants run slower than Speed rather than bursting, as the plugin’s do below 10 fps.',
    'Over: the clip is the kit’s generated one (or your own file), premultiplied, read ONCE when the field is seeded (and on Reseed or a Pattern change), as in the plugin. The plugin reads its R32UI sample back as RED_INTEGER; WebGL2 guarantees only RGBA_INTEGER, so the page reads that where RED_INTEGER is not offered — the same values. The grid follows the output’s aspect, so the 16:9 Composition presets all walk the same grid; a file of another shape re-grids, keeping the middle.',
    'Nothing here is measured. The plugin’s harness finds Langton’s highway from step 9977 with its 104-step period, Turk and Propp’s pair rules mirror-symmetric at every return home and LRRRRRLLR filling its square, holds the plugin’s walk to a second walk written separately, reads the GPU’s state texture back every frame, and runs every rule out and back exactly, each check with a negative control — and that harness, not this page, is the reason to believe the walk. The plugin itself has never been loaded into Resolume.',
  ],

  createRenderer: (gl, quad) => {
    renderer = createRenderer(gl, quad);
    return renderer;
  },
});

//---------------------------------------------------------------------------
// Which controls belong to which plugin, and whose defaults are in force.
//
// By inline style, not the `hidden` attribute: kit.css gives these elements a
// `display` of their own, which beats the attribute's user-agent rule. Rows are
// found by their control's id (the two Pattern rows share a name), falling back
// to the name for the toggles, which carry no id.
//---------------------------------------------------------------------------
if (mounted) {
  const params = mounted.params;
  const embed = query.has('embed') && query.get('embed') !== '0';
  let shownVariant = mounted.state.variant;

  // Copy link carries the plugin, since the two declare different controls.
  const toQuery = params.toQuery.bind(params);
  params.toQuery = () => {
    const q = toQuery();
    if (mounted.state.variant === 'over') q.set('plugin', 'over');
    return q;
  };

  const show = (node, on) => { if (node) node.style.display = on ? '' : 'none'; };
  const rowOf = (p) => document.getElementById(`p-${p.id}`)?.closest('.prow')
    ?? [...document.querySelectorAll('.prow')].find((row) => row.querySelector('.prow__name')?.textContent === p.name);

  function showForVariant(variant) {
    for (const p of PARAMS) {
      if (p.only) show(rowOf(p), p.only === variant);
    }
    const effect = variant === 'over';
    for (const field of document.querySelectorAll('.transport__field')) {
      if (field.querySelector('.transport__label')?.textContent === 'Clip') show(field, effect);
    }
    show(document.querySelector('.transport__file'), effect);
  }

  document.addEventListener('demo:state', () => {
    const variant = mounted.state.variant;
    if (variant !== shownVariant) {
      shownVariant = variant;
      // A new instance: that constructor's defaults, for Defaults too.
      params.defaults = { ...DEFAULTS[variant] };
      params.reset();
    }
    if (!embed) showForVariant(variant);
  });

  if (!embed) {
    showForVariant(shownVariant);

    //-----------------------------------------------------------------------
    // A status line: reports, measures nothing.
    //-----------------------------------------------------------------------
    const stage = document.querySelector('.stage');
    if (stage) {
      const line = document.createElement('p');
      line.className = 'stage__status';
      line.id = 'langton-stats';
      line.setAttribute('aria-live', 'off');
      stage.append(line);
      setInterval(() => {
        const s = renderer?.stats;
        if (!s) return;
        line.textContent =
          `${s.speed === 0 ? 'Paused (Speed 0)' : `${sig(s.speed)} rounds a second`}${s.reversed ? ', backwards' : ''}`
          + ` · ${s.ants} ant${s.ants === 1 ? '' : 's'} on ${s.rule}`
          + ` · grid ${s.grid.cols} × ${s.grid.rows}`
          + ` · ${grouped(s.net)} round${Math.abs(s.net) === 1 ? '' : 's'} from the seed`
          + ` · ${grouped(s.steps)} ant steps taken`;
      }, 250);
    }
  }
}
