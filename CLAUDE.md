# langton

Langton's ant for Resolume Arena/Avenue, as two FFGL plugins from one core:
`SW Langton` (`LA01`, source: ants on an empty grid or a soup) and `SW Langton
Over` (`LA02`, effect: the clip's brightness is the ground the ants walk).
C++/GLSL, CMake MODULE → two universal `.bundle`s (macOS) + Windows `.dll`s. MIT.
Bundle ids `com.stoatworks.ffgl.langton` and `com.stoatworks.ffgl.langton.over`.

Read `AGENTS.md` before touching the walk (`Ant.h/.cpp`), the state texture's
layout, the upload, the composite or the parameter tables.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (never from `~/Projects`)
- Render the source: `./build/lntest --out /tmp/ant.png --frames 1300` (the highway)
- Render the Over effect on the harness's card: `./build/lntest --over --out /tmp/o.png`
- A frame of a real clip through it: `ffmpeg -i clip.mov -frames:v 1 -s 1280x720 -f rawvideo -pix_fmt rgba /tmp/c.rgba && ./build/lntest --over --clip /tmp/c.rgba --out /tmp/o.png`
- List parameters: `./build/lntest --list` (`--over` for the effect's)
- Set anything by name: `./build/lntest --set "Rule=3" --set "Speed=0.6"` (options by element index)
- The source as video: `./build/lntest --pipe --frames 1800 --size 1280x720 --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- A clip through the effect: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba -s WxH - | ./build/lntest --over --pipe --size WxH | ffmpeg …`
  (Resolume's demo clips are 30 fps; the harness clocks every frame at 1/60 s, so
  resample with `fps=60` or the ants walk at half speed against the picture.)
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the same
  units as `--set`. Held before the first key and after the last; a standard
  control is linear between keys, and an option, boolean, event or integer
  STEPS (holds each key until the next key's frame), so a press is three keys
  (0, 1, 0). Frame *n* is clocked at n / `--fps` (60). An unknown name exits 2
  before any frame; a partial frame at EOF ends the stream with exit 0; a reader
  that hangs up ends `--pipe`/`--film` with exit 1 (SIGPIPE is ignored), never a
  silent 141. The source's `--pipe` with no `--frames` runs until the reader
  hangs up. The source's frames are written over black (it is premultiplied,
  transparent where nothing is painted).
- A draft of the fleet gate's expectation from what the plugins declare:
  `./build/lntest --expect` (a draft only: its `note` fields are not the gate's).

## Verify
- Everything: `tools/verify.sh` (~1.5 min: reserved words, no unbounded trig in the
  GLSL, glslc, fresh universal build, both bundles through
  lipo/plist/codesign/oxbow probe+selftest, every check at 1280x720 and 320x180,
  the same checks on Apple's software renderer at 320x180, the offline set, the
  `--pipe` contract, the negative controls, the mutants, the sweep, the bench)
- **The literature**: `--literature` (no GL: the plugin's walk against Langton's
  highway, Turk and Propp's symmetry and square, and reversibility), `--engine` (no
  GL: the walk against the harness's own, cell for cell).
- **Through the plugin**: `--highway` (the GPU's texture is the walk, every frame),
  `--reverse` (every rule out and back exactly), `--clock` (Resolume's 499 million
  ms), `--coverage` (the box filter), `--look` (palette, trail, ants), `--over-check`,
  `--prime`, `--resize`, `--state`; no GL: `--clock-law`, `--cues`, `--names`.
- One raster only: add `--size WxH`. The software renderer:
  `LNTEST_RENDERER=software ./build/lntest --highway --size 320x180`.
- **The checks can fail**: `--negative` (14 wrong models), `tools/mutate.sh`
  (one character of GLSL, Ant.cpp, Langton.cpp and Clock.h).
- What CI runs: `--offline` and `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (35 parameters over both plugins).
- Cost: `--bench` (720p/1080p/4K, both plugins, and 1080 rows × 8 ants × 1e6/s).

## Notes
- **The walk is on the CPU, exact** (`ant::World`): no GL, so `--offline` checks
  it in CI. Each frame the rows it touched go up into an RG32UI texture: R the
  colour | 0x100 once visited, G the activity clock at the last visit. The
  composite only draws.
- **Headings are clockwise on screen from up**, and rows count UP (GL's order):
  R turns up → right. The harness's own walk uses direction vectors, not the
  heading table, so the two are separate readings of the rule.
- **The step is a bijection**: backward() moves back, steps the colour back and
  undoes that colour's turn; reversed rounds take the ants in reverse order.
- **The grid follows Cell Size and the raster's ASPECT, not its size.** A
  re-grid keeps the overlap, centred, with offsets TRUNCATED toward zero (not
  floored), so 16:9 → 4:3 → 16:9 puts every cell back. Ants keep their place
  relative to the middle and wrap.
- **A Rule change keeps the field** (colours read modulo the new length, by the
  walk AND the composite: a cell left over from a longer rule draws as the colour
  the ants take it for; before 2026-10-20 it drew as an empty palette slot,
  opaque black, and `--look` now checks it); an Ants
  change puts the ants back at their start places on the same field; a Seed
  change or a Pattern change reseeds; Density acts from the next reseed.
- **Probabilities are integer thresholds** (`ThresholdU32`): a PCG hash below
  p × 2³² fires, never a float compare.
- **Host indices are not ParamIds.** Each plugin declares its own dense list
  (`HostOrder()`), About block LAST in both (radar's shape).
- Every host parameter is 0..1 except Ants and Seed (real integers).
  An option reads back 0..1 whatever its count: map by element index.
- No sin/cos/atan in the GLSL (verify.sh greps): the hue wheel is clamped
  triangles. Randomness is PCG integer hashing, never `fract(sin(...))`.
- GLSL reserved words must not be identifiers (including `packed`, `smooth`,
  `sample`); verify.sh greps for the 4.10 list. No `M_PI`, no `far`/`near`.
- `langton_core` is an OBJECT library: the registrations are file-scope
  constructors nothing references.
- Not yet public, not released, not registered on the website.

## Browser demo
- `demo/` is the page at https://langton-demo.stoatworks-labs.com (Cloudflare
  Worker `langton-demo`, a ROUTE on a proxied AAAA 100:: record -- the zone's
  custom domains are full; deleting the record takes the page dark with a green
  deploy). `deploy.yml` redeploys it on a push to main; by hand:
  `cf-run npx wrangler deploy`.
- `demo/shaders.js` is GENERATED: after changing `source/Shaders.cpp` run
  `python3 demo/tools/check_shaders.py --write`; verify.sh fails on drift, on a
  new piece the check does not list, and on a change to how pieces are joined.
- The walk in `demo/plugin.js` (between the PORT markers: Ant.cpp, Hash.h,
  Clock.h, Controls.cpp) is held to the plugin's C++ by
  `demo/tools/crosscheck/crosscheck.sh` (verify.sh runs it; needs node): change
  `Ant.cpp` and the port with it. The frame logic below the markers (Langton.cpp's
  ProcessOpenGL, seed, upload, composite) only a reader checks.
- `demo/vendor/` is the shared kit: never edit it, re-vendor with
  `stoatworks-backend/resolume-demo/sync.sh`.

## Not done yet
- Never loaded into Resolume on macOS (oxbow selftest only). On Windows it is gated
  in Arena 7.27.1 on win-lab (software rendering, no sound): expectation
  `~/Projects/resolume/plugin-bench/arena/expect/langton.json`. No OpenFX port, no
  presets.
- Released v0.1.0 (2026-10-20). `StoatworksAbout.h`, `ATTRIBUTIONS.md`,
  `.github/FUNDING.yml` and `.github/ISSUE_TEMPLATE/` are GENERATED by the
  backend's sync scripts: never edit them here. `docs/USER-GUIDE.md` is the only
  copy of the guide anyone edits; its PDF and the site page come from the
  website's `scripts/build_guides.py langton` (always name the slug).
- Windows builds set `-DVCPKG_APPLOCAL_DEPS=OFF`: the two plugins' applocal copy
  steps collide under `--parallel` (see release.yml).

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs
inside Resolume). It records which shader failed to compile and the GL
vendor/renderer.

    ~/Library/Logs/langton/langton.YYYY-MM-DD.log
