# Attributions

Langton is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Source-plus-Over shape, parameter table, GL state and harness — Stoatworks radar

<https://github.com/stoatworks-labs/radar>  
Licence: MIT  
Copyright: Stoatworks Labs

One core registered as a source and an Over effect, HostOrder() with the About block last, the host-clock unit vote, GLState.h, Diag, the harness's rig, cue sheets and --pipe, tools/verify.sh, tools/mutate.sh, tools/glslc.sh and tools/sweep.py are radar's (which carries them from boreal, flyback, downpour, tinsel and plotter).

### Box-filtered composite, clock, harness and the Over's integer clip mapping — Stoatworks conway

<https://github.com/stoatworks-labs/conway>  
Licence: MIT  
Copyright: Stoatworks Labs

The two-plugin shape, the step clock (Clock.h), the box-filtered composite, the Over's integer pixel mapping, the Seed-before-Reseed button order and most of the harness (its rig, cue sheets, --pipe, --bench, --negative) are conway's, adapted from generations to ant steps.

### Audio analyser — Stoatworks rosette

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is conway's copy of radar's copy of millpond's copy of rosette's analyser (itself from macroblock's), with its primed first frame, which lntest --prime checks.

### An integer-format PassBuffer — Stoatworks stencil

<https://github.com/stoatworks-labs/stencil>  
Licence: MIT  
Copyright: Stoatworks Labs

source/PassBuffer.{h,cpp} is stencil's, cut down: a texture and a framebuffer whose upload format follows an integer internal format, which ffglex::FFGLFBO cannot allocate. Here it gains an RG32UI format and a row upload.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Langton's ant — Wikipedia

<https://en.wikipedia.org/wiki/Langton%27s_ant>

Checked 2026-10-04 for the behaviour the harness asks of the plugin: on an empty grid Langton's ant makes a chaotic pattern for about 10,000 steps and then builds a recurrent "highway" of 104 steps that repeats indefinitely (the harness measures the onset at step 9977 and the drift at two cells diagonally per cycle); Turk and Propp's many-coloured ants, and the article's examples, which are the Rule dropdown: RLR grows chaotically, LLRR and RLLR symmetrically, LRRRRRLLR fills a square, LLRRRLRLRLLR builds a convoluted highway, and RRLLLRLLLRRR a filled triangle that starts to move after about 15,900 steps (the harness checks the symmetric pair and the square; the rest it does not); and the observation that rules made of consecutive pairs of equal letters give symmetric patterns, which the harness checks at every return to the start.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Langton's ant

Christopher Langton's ant (1986), and Greg Turk's and Jim Propp's generalisation to many colours. Implemented from the rule's definition; nothing is copied from anyone's source.

## Standards and published specifications

What the implementation is measured against.

- **Turk and Propp's rule strings** — one letter per colour, L or R: on colour c turn by letter c, advance the cell to colour c + 1 (mod n), step forward. Langton's ant is RL.
- **Melissa E. O'Neill, PCG (2014)** — the integer hash behind every soup, compared with integer thresholds in the plugin and the harness.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
