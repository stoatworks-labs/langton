#!/usr/bin/env python3
"""Render every parameter at both ends of its range, in both plugins, and fail
if any made no difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act: the audio controls with a beat fed in,
Clip Colour, Backdrop and Mix through the Over effect on the harness's
night-street card, Density, Seed and Reseed on a soup. An option parameter reads back 0..1 whatever its
count (the fleet's trap), so options are set here by element index. A control whose ends differ by less than
`--floor` (mean 8-bit difference per channel) is reported as barely alive:
vectrix's lesson, a control can be alive, correct and still useless.

Every render is 320x180 (CI's raster) and 160 frames at 60 fps: 1600 rounds at
the source's default 600 a second, still in Langton's chaos, long before the
highway.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N]
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# name -> (low setting, high setting, context settings, extra flags)
SWEEP = {
    "Rule": ("0", "3", [], []),
    "Ants": ("1", "8", [], []),
    "Speed": ("0.3", "0.6", [], []),
    "Reverse": ("0", "1", [], []),
    # One press is one round: big cells, or a single cell is too small to see.
    "Step": ("0", "1", ["Speed=0", "Cell Size=1"], []),
    "Pattern": ("0", "1", [], []),
    # Density, Seed and Reseed act on a soup; the source's default is empty
    # ground and the Over's is the clip, which they do not touch.
    "Density": ("0.1", "0.6", ["Pattern=1"], []),
    "Seed": ("1", "2", ["Pattern=1"], []),
    "Reseed": ("0", "1", ["Pattern=1"], []),
    "Audio Burst": ("0", "1", ["Speed=0"], ["beat"]),
    "Audio Reverse": ("0", "1", [], ["beat"]),
    "Cell Size": ("0.3", "0.7", [], []),
    "Gap": ("0", "1", [], []),
    "Palette": ("0", "4", [], []),
    "Trail": ("0", "1", [], []),
    # The ants are a cell each: big cells and all eight of them.
    "Show Ants": ("0", "1", ["Ants=8", "Cell Size=0.9"], []),
    "Clip Colour": ("0", "1", [], []),
    "Backdrop": ("0", "1", [], []),
    "Mix": ("0", "1", [], []),
}

# Parameters with no pixel to sweep: the FFT buffer (its float is meaningless;
# Audio Burst and Audio Reverse are its sweepable proof) and the About block
# (a text line and browser buttons).
SKIP = {"Audio", "About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for lntest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(lntest, out, settings, extra, effect):
    args = [str(lntest), "--out", str(out), "--size", "320x180", "--frames", "160"]
    if effect:
        args.append("--over")
    if "beat" in extra:
        args.append("--beat")
    for setting in settings:
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"lntest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(lntest, effect):
    result = subprocess.run([str(lntest), "--list"] + (["--over"] if effect else []), capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"lntest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(lntest, scratch, effect, name, declared):
    low, high, context, extra = SWEEP[name]
    # A context names controls of either plugin; each keeps its own.
    context = [c for c in context if c.split("=")[0] in declared]
    tag = f"{'o' if effect else 's'}{abs(hash(name))}"
    a = scratch / f"{tag}-a.png"
    b = scratch / f"{tag}-b.png"
    before = render(lntest, a, context + [f"{name}={low}"], extra, effect)
    after = render(lntest, b, context + [f"{name}={high}"], extra, effect)
    return effect, name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    lntest = build / "lntest"
    if not lntest.exists():
        print(f"{lntest} not found", file=sys.stderr)
        return 1

    jobs, names = [], {}
    for effect in (False, True):
        declared = parameters(lntest, effect)
        names[effect] = set(declared)
        unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
        if unknown:
            # A new parameter with no sweep is a hole, not a pass.
            print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
            return 1
        jobs += [(effect, n) for n in declared if n in SWEEP]
    unused = [n for n in SWEEP if not any(n == j[1] for j in jobs)]
    if unused:
        print(f"the sweep names parameters neither plugin has: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda j: sweep_one(lntest, scratch, j[0], j[1], names[j[0]]), jobs))
    for effect, name, delta in results:
        who = "SW Langton Over" if effect else "SW Langton"
        if delta == 0.0:
            dead.append(f"{who}: {name}")
            print(f"  DEAD {who:16s} {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(f"{who}: {name}")
            print(f"  WEAK {who:16s} {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {who:16s} {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept over both plugins, {len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
              "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
