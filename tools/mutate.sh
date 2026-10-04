#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run.
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
MUTANTS=(
	"source/Shaders.cpp|vec2 b = min( hi, vec2( x + 1, y + 1 ) - inset );|vec2 b = min( hi, vec2( x + 1, y + 1 ) + inset );|--coverage|GLSL: the painted square grows past its cell on one side, - -> +"
	"source/Shaders.cpp|Level   = min( uint( Colours - 1 ), uint( floor( b * float( Colours ) ) ) );|Level   = min( uint( Colours - 2 ), uint( floor( b * float( Colours ) ) ) );|--over-check|GLSL: the clip's top level clipped off, 1 -> 2"
	"source/Shaders.cpp|float glow = exp( -float( Now - s.g ) / Trail );|float glow = exp( +float( Now - s.g ) / Trail );|--look|GLSL: the trail grows with age instead of fading, - -> +"
	"source/Ant.cpp|colour[ i ]        = static_cast< uint8_t >( ( c + 1 ) % n );|colour[ i ]        = static_cast< uint8_t >( ( c + 2 ) % n );|--engine|C++: a step advances the cell two colours, 1 -> 2"
	"source/Ant.cpp|const int c    = ( colour[ i ] % n + n - 1 ) % n;|const int c    = ( colour[ i ] % n + n - 2 ) % n;|--reverse|C++: the way back steps the colour back two, 1 -> 2"
	"source/Langton.cpp|const size_t from = static_cast< size_t >( first + y ) * cols + x;|const size_t from = static_cast< size_t >( first - y ) * cols + x;|--highway|C++: the upload reads the rows downwards, + -> -"
	"source/Clock.h|owed -= whole;|owed += whole;|--clock-law|C++: the clock banks the rounds it ran, -= -> +="
)

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target lntest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	if "$tree/build/lntest" "$check" >"$WORK/log" 2>&1; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant:\n' "$check"
		grep -E '^  FAIL' "$WORK/log" | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
