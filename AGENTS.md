# langton — for agents

The why behind the code. `CLAUDE.md` has the commands; this file has the
reasoning, the traps that were actually hit, and what is and is not known.
Built 2026-10-04 in one session, from Allan's one-line request ("another
resolume plugin that plays langton's ant"), straight after conway, whose
shape it reuses. The spec is `~/Projects/resolume/specs/SPEC-langton.md`; the
briefs are `~/Projects/resolume/specs/BRIEF.md` and `BRIEF-ADDENDUM.md`.

## The one idea

**An ant on a grid turns right on a white cell and left on a black one, flips
the cell and steps forward.** Langton's ant (1986). Generalised by Turk and
Propp to n colours with a rule string of L and R, one letter per colour: turn
by the letter for the cell's colour, advance the cell's colour by one (mod n),
step. Nothing is drawn as a picture of a walk: the walk is the rule, run
exactly, and the chaos, the highway at step 9977, the symmetric growth of the
pair rules and LRRRRRLLR's square fall out of it.

The house-style claim is that the plugin IS the ant, so the harness asks the
plugin's own walk for the published behaviour (the highway's onset, period and
drift; the pair rules' symmetry; the square) and holds it, cell for cell, to a
walk the harness wrote separately; then it reads the GPU's state texture back
and finds the same walk there.

## How a frame goes

1. **Time** (`Langton.cpp`, `Clock.h`): the host's elapsed time, frame to frame,
   in double (the unit voted against the wall clock, rosette's code via radar
   and conway); a jump (backwards, or over 1 s) passes no time and resets the
   audio analyser. `StepClock` owes Speed × dt rounds and carries the fraction,
   so the count is floor( Speed × elapsed ) at any frame rate.
2. **Audio**: the primed onset analyser; an onset adds Audio Burst rounds and,
   with Audio Reverse, flips the direction of time. Audio Reverse off forgets
   the flips, so time follows Reverse again (it used to latch: found while
   writing the guide, 2026-10-20; `--prime` checks it).
3. **Buttons**: Step adds a round on its rising edge. Seed is read BEFORE
   Reseed (conway's sweep found the other order swallows a press); a Seed or
   Pattern change reseeds.
4. **Grid**: rows from Cell Size, columns from the raster's aspect. A new shape
   re-grids the walk (the overlap kept, centred).
5. **Allocate** the RG32UI state (and the Over's R32UI sample target) before
   anything is bound; a new texture marks every row dirty.
6. **Seed** if needed: reset, the rule, the ants at their start places, then
   the ground: empty, a soup (PCG hash per cell under an integer threshold), or
   the clip (the sample pass quantises the clip's brightness at each cell's
   centre into the rule's levels; one `glReadPixels` of an R32UI target).
7. **Rule / Ants changes**: a new rule carries the field on; a new ant count
   re-places the ants on the same field.
8. **Walk**: `min( owed + presses + onsets, 100,000 )` rounds, forward or back
   (Reverse XOR the audio's flip), on the CPU.
9. **Upload** the rows the walk touched (one `glTexSubImage2D` of the dirty row
   range) and **composite**.

### The walk (`Ant.h`, `Ant.cpp`)

A step at colour c: heading += turn[ c ] (mod 4), colour = c + 1 (mod n),
visited = 1, visit = the activity clock, move one cell, wrap (one add or
subtract per axis: an ant never moves more than one cell). The inverse: move
back, wrap, c = colour − 1 (mod n), colour = c, heading −= turn[ c ]. A
reversed round takes the ants in the opposite order, so n rounds out and n
back are the identity on every cell and every ant, whatever the ants did to
each other's cells. `--literature`, `--engine` and `--reverse` all hold this.

The activity clock counts ant steps either way, so the trail glows where the
ants walk backwards too. It is a uint64 on the CPU and its low 32 bits on the
GPU; the composite subtracts in uint, so the age survives the wrap.

### The state texture

RG32UI, one texel per cell: R = colour | 0x100 once visited, G = the clock at
the last visit. The GPU never computes the walk; it only reads this. That is
why `--highway` compares the texture with the harness's walk after every
frame: the upload of a dirty row range is the one place the GPU's picture and
the CPU's walk can part.

### The composite

Conway's box filter: each pixel is the cells it overlaps, each weighted by the
exact area of overlap with its painted square (the cell inset by Gap / 2). A
cell is the palette's colour for its own (colour 0 is nothing; the source is
premultiplied, transparent there), plus TrailColour × exp( −age / Trail ) if
visited, and AntColour where an ant stands. `--coverage` checks the filter
(Mono, no trail, no ants: every painted cell is white), `--look` the colours
(cells of exactly 10 x 10 pixels, so the pixel at each cell's middle is that
cell alone).

### The Over

The clip is the ground ONCE, at the seed: its brightness (the brightest channel
× alpha, radar's lesson: a saturated blue has luma 0.07) at each cell's centre,
level k of n covering [ k/n, (k+1)/n ). The pixel is found in integer
arithmetic, ( 2c + 1 ) raster / ( 2 grid ), so which pixel decides a cell is
exact on any GPU. After that the ants repaint it; the clip shows through as
Backdrop, Clip Colour and Mix. Feeding the clip in continuously (conway's Feed)
was rejected: it would overwrite the ants' work every frame and there would be
no walk to see.

## Decisions taken without asking

- **Name and ids**: `langton`, `SW Langton` / `SW Langton Over` (10 and 15
  bytes), `LA01` / `LA02` (checked free against every `CFFGLPluginInfo` in the
  fleet and `~/dev` on 2026-10-04).
- **A source and an Over effect** (conway's shape), each declaring only its own
  controls, About block last in both.
- **The walk on the CPU**, not in a shader. An ant's step depends on the step
  before it, and several ants share cells, so a GPU walk is one thread doing
  serial work plus a readback; the CPU walk is exact, checkable without GL, and
  at the default speeds costs nothing. The price is the worst case: a million
  rounds a second with eight ants is ~3.4 ms a frame here.
- **Seven rules**: Langton's RL and Wikipedia's six many-coloured examples (LLRR,
  RLLR, LRRRRRLLR, LLRRRLRLRLLR, RRLLLRLLLRRR, RLR), by their strings.
- **Up to eight ants**, taking turns; the first in the middle facing up (so the
  default reproduces Langton's walk exactly), the rest on a circle round it,
  each facing the next heading round.
- **A torus**, not dead edges: an ant at an edge must go somewhere, and the
  torus keeps the step a bijection.
- **Defaults**: the source is Langton's own ant on an empty grid at 600 steps a
  second, so the highway appears at ~16.6 s; 120 rows; Gap 0.2; Phosphor;
  Trail 3000 steps. The Over: four ants at 3000 a second on the clip.
- **Speed is geometric**, 1 to 10⁶ rounds a second (0 pauses); Audio Burst 10 to
  10⁵ rounds; Trail 10 to 10⁶ steps (0 off). Per frame at most 10⁵ rounds, the
  rest dropped, not banked.
- **Commit trailer** is this session's model, `Claude Opus 5.5`.

## The traps

**The re-grid's offsets must truncate, not floor.** The first `Regrid` centred
with floor division, so 213 → 160 → 213 columns moved the field by one cell:
floor( 53 / 2 ) = 26 out, floor( −53 / 2 ) = −27 back. Found while writing
`--resize`'s expectation, before it ran. Truncation makes the way back the
way there negated.

**The reallocation test must come before `Ensure`.** The first draft asked
whether the state texture had changed size after allocating it, which is
always "no", so a re-grid would have drawn stale rows until the ants touched
them.

**`World()` is a member function and `ant::World` a type**; inside
`LangtonPlugin` the unqualified name is the function. Always write `ant::World`.

**Conway's traps, respected here**: integer formats need an integer-aware
PassBuffer (stencil's, now with an RG32UI format and a row upload); allocate
before binding; units bound by hand and released; a sampler on texture 0 is
"unloadable" (the source binds a 1x1 blank to its clip sampler);
`SetTextParameter` for About; OBJECT library; integer hashing only; the
clock-unit vote and double time; primed onsets; `GL_COLOR_WRITEMASK` restored;
no `M_PI`, `<cmath>` included, no `far`/`near`; the 4.10 reserved-word list; an
option reads back 0..1 whatever its count; unique names as Arena addresses them;
output alpha `mix( clip, 1, Mix )`; `clamp` never with lo > hi.

## Would this hold on another rasteriser, at another raster?

Every check runs at 1280x720 and at 320x180 on this Mac's GPU, and at 320x180
on Apple's software renderer (`verify.sh`). CI runs only `--offline` and
glslc: a macOS runner has no accelerated GL.

| check | bound | why that number | raster / rasteriser |
| --- | --- | --- | --- |
| `--literature`, `--engine` | exact | integer walks on the CPU | CPU |
| `--highway` | texel for texel | the texture is integer, uploaded and read back as integers | the grid is set by the test, so raster-free; both rasterisers |
| `--reverse` | exact | integer state through the plugin | both rasterisers |
| `--clock-law` | exact | the clock in double; checkpoints within 1e-6 of a whole round are not scored (6 of 36,000) | CPU |
| `--clock` | dt within 1e-9 s; the count exact | a double ulp at 4.99e8 ms is 6e-8 ms; measured 6.2e-11 s | raster-free |
| `--coverage` | 4 × ulp( max( cols, rows ) ) / min( cells per pixel ) + 4 × 2⁻²³ | every footprint edge is a float at up to the grid's size, four edges per pixel | measured 2.9e-5–1.9e-4 (GPU), inside each bound; 0 at half-pixel cells |
| `--look` | 2e-5 | GLSL `exp` and float colours against double | measured 7.5e-6 |
| `--over-check` | exact cells; Mix 0 bit for bit | integer pixel mapping; `texelFetch` and `mix( a, b, 0 ) = a` | both rasters, both rasterisers |
| `--prime`, `--resize`, `--state`, `--names`, `--cues` | exact | counts, integer state, GL state | `--resize` crosses 1280x720 ↔ 320x180 ↔ 960x720 |

## A check that cannot fail is not a check

`lntest --negative` sets one wrong MODEL at a time and requires the check to
fail. All 14 are detected:

| check | the wrong model |
| --- | --- |
| `--literature` | Langton's ant read mirror-image (LR), the pair rules with a letter changed (LLRL) |
| `--engine` | the torus wraps one cell short on the left and bottom (the plugin's walk) |
| `--highway` | the upload skips the last row each frame touched |
| `--reverse` | the way back undoes each turn the wrong way |
| `--clock-law` | the owed fraction carried in float |
| `--clock` | elapsed time from the host clock in float |
| `--coverage` | point-sample the cell under each pixel's centre |
| `--look` | the harness expects the trail to fade by rounds, not ant steps |
| `--look` | the harness expects a cell painted under a longer rule drawn as its own colour, not modulo the rule |
| `--over-check` | the clip laid under the grid mirrored |
| `--prime` | the analyser unprimed |
| `--prime` | the harness expects an onset's turn to outlast Audio Reverse switched off |
| `--resize` | the walk cleared on a resize |
| `--cues` | every control ramps between keys |

The mirror-image ant is a good wrong model because so much of it is right: its
turns are 104-periodic from step 9977 too, and it paints 720 cells by 10,000.
Only the drift (+2, −2) and the bounding box give it away.

### The recorded mutation

`tools/mutate.sh` (run by verify.sh) changes one character of shipped code in a
copy of the tree and requires a named check to fail. All 7 are caught: in the
GLSL the painted square grown past its cell (`--coverage`), the clip's top level
clipped (`--over-check`), the trail growing with age (`--look`); in the C++ a
step advancing the colour by two (`--engine`), the way back stepping the colour
back by two (`--reverse`), the upload reading rows downwards (`--highway`), and
the clock banking the rounds it ran (`--clock-law`). The harness has no copy of
the shaders or the walk: it drives the plugin's.

## Shape of the code

    source/Ant.{h,cpp}        the walk: rules, ants, step, inverse, soup, re-grid
    source/Langton.{h,cpp}    the plugin: time, audio, buttons, seed, upload, composite
    source/Shaders.{h,cpp}    the sample (Over) and composite passes
    source/Controls.{h,cpp}   parameter ids, names, groups, ranges, HostOrder
    source/Clock.h            the step clock
    source/PassBuffer.*       an integer-format texture + FBO, with a row upload
    source/Audio.*, GLState.h, Hash.h, Diag.*   the fleet's
    tools/lntest/main.cpp     the harness
    tools/{verify,mutate,glslc}.sh, tools/sweep.py

## What is genuinely verified, and what is assumed

Verified, on this Mac: everything in the README's table, at two rasters, on the
GPU and on Apple's software renderer; both bundles universal, ad-hoc signed,
read by a host (oxbow) and rendered through.

Assumed: that Resolume calls the plugin as oxbow does; the clock unit on
macOS; the FFT bins in a real host; anything on Windows (never built there).
The palettes are chosen by eye.

## Open design questions

- Should the Over be able to re-read the clip continuously, as a slow "Feed"
  that only touches cells no ant has visited? Rejected for v0.1.0 (it muddies
  Reverse); worth trying if people ask.
- Dead edges (an ant that leaves dies, or turns round) would end the torus's
  highway-into-debris, but break the bijection unless designed carefully.
- More ants than eight would want the walk in a worker thread.
