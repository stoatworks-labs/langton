#!/usr/bin/env bash
#
# The browser demo's JavaScript port of the walk, against the plugin's C++.
#
#     demo/tools/crosscheck/crosscheck.sh        exit 1 on any difference
#
# printer.cpp drives the plugin's own GL-free sources (Ant.cpp, Controls.cpp,
# Hash.h, Clock.h) through a fixed scenario -- every rule, 1 to 8 ants, empty
# grids and soups, ants starting on the edges, thousands of rounds out and the
# same back, rule changes, re-grids, loads, every slider position's conversion,
# the hash, the seed's salt and the step clock -- and prints it, floats as their
# bits. run.mjs slices the PORT block out of demo/plugin.js and prints the same
# scenario. The two must be identical. Run by tools/verify.sh.
#
# Two builds of the printer, because of ONE fused multiply-add. Apple clang
# contracts `owed += stepsPerSecond * dt` in Clock.h into a single fmadd on
# arm64 (its default -ffp-contract=on), so the plugin's arm64 slice carries the
# clock's fraction of a round with one rounding fewer than JavaScript does.
# x86_64 has no FMA in its baseline, so the plugin's Intel slice computes it
# exactly as JavaScript does. So:
#
#   1. contraction off (the Intel slice's arithmetic): every line identical;
#   2. this Mac's native build, as the plugin is built: identical except the
#      clock's carried fraction, which is masked -- every round count, total and
#      per-frame digest still has to match.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
CXX="${CXX:-c++}"

command -v node >/dev/null 2>&1 || { echo "crosscheck: node is not installed"; exit 2; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

sources=( "$HERE/printer.cpp" "$REPO/source/Ant.cpp" "$REPO/source/Controls.cpp" )
"$CXX" -std=c++17 -O3 -ffp-contract=off -I "$REPO/source" "${sources[@]}" -o "$tmp/printer-strict"
"$CXX" -std=c++17 -O3 -I "$REPO/source" "${sources[@]}" -o "$tmp/printer-native"

"$tmp/printer-strict" > "$tmp/cpp-strict.txt"
"$tmp/printer-native" > "$tmp/cpp-native.txt"
node "$HERE/run.mjs" > "$tmp/js.txt"

lines=$( wc -l < "$tmp/js.txt" | tr -d ' ' )
[[ "$lines" -gt 10000 ]] || { echo "FAIL the JavaScript side printed only $lines lines"; exit 1; }

if ! diff -q "$tmp/cpp-strict.txt" "$tmp/js.txt" >/dev/null; then
	echo "FAIL the JavaScript port differs from the plugin's C++ (contraction off):"
	diff "$tmp/cpp-strict.txt" "$tmp/js.txt" | head -12 | sed 's/^/      /'
	exit 1
fi
echo "ok   $lines lines identical: the port against the plugin's C++ with contraction off (the x86_64 slice's arithmetic)"

# The clock's carried fraction, and only that: field 5 of a `clock` line, the
# owed= of a `clockcase` line, fields 6 and 8 of the `clockedge` line.
mask() {
	awk '$1 == "clock" { $5 = "~" }
	     $1 == "clockcase" { sub(/owed=[0-9a-f]+/, "owed=~") }
	     $1 == "clockedge" { $6 = "~"; $8 = "~" }
	     { print }' "$1"
}
mask "$tmp/cpp-native.txt" > "$tmp/cpp-native.masked"
mask "$tmp/js.txt" > "$tmp/js.masked"
if ! diff -q "$tmp/cpp-native.masked" "$tmp/js.masked" >/dev/null; then
	echo "FAIL the JavaScript port differs from this Mac's native build of the plugin's C++ beyond the clock's fraction:"
	diff "$tmp/cpp-native.masked" "$tmp/js.masked" | head -12 | sed 's/^/      /'
	exit 1
fi
fused=$( diff "$tmp/cpp-native.txt" "$tmp/js.txt" | grep -c '^<' || true )
echo "ok   against the native $(uname -m) build: identical but for $fused clock line(s) whose carried fraction differs in its last bits (the fused multiply-add); every round count identical"
