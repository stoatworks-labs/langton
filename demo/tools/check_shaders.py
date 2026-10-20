"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the four GLSL pieces of `source/Shaders.cpp` (the
kCommon library, kQuadVertex, and the sample and composite bodies) plus
kVersion. That is two copies of the same text, and two copies drift --
quietly, because a walk drawn by a *plausible* composite looks exactly like one
drawn by the right one. The whole claim of the page is that it runs the
plugin's own shaders, so the claim needs something enforcing it. `lntest`
drives the real plugin class and has never heard of this page, and
`tools/glslc.sh` compiles the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of `source/Shaders.cpp` and the matching
backtick literal out of `demo/shaders.js`, and compares them exactly -- no
whitespace normalisation, no comment stripping: a comment updated on one side
only is exactly the drift worth catching. `kVersion`, a plain string, is
compared too.

It also fails when Shaders.cpp grows a piece this script does not list (a new
pass the page would silently lack), and it checks the JOIN as well as the
pieces: `Assemble()` in Shaders.cpp is kVersion + kCommon + the piece, and
InitGL in Langton.cpp builds the vertex stage as kVersion + kQuadVertex and the
two fragment stages with Assemble(); plugin.js must assemble them the same way.
A change to either side's assembly fails here and needs a reader.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a template literal, so --write would escape one as \\`. There
are none in the C++ bodies today. This undoes that escape and REJECTS any other
backslash or any `${` on the JS side; there are none in the C++ bodies, so
either could only be somebody hiding a difference.

`--write` produces shaders.js from the C++ by exactly that escape, so the copy
is spliced by this script and never typed.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half. The walk (Ant.cpp's `ant::World`, the
plugin's CPU engine), the hash (Hash.h), the clock (Clock.h), Controls.cpp's
conversions and the frame logic of Langton.cpp's ProcessOpenGL, seed(),
upload() and composite() in demo/plugin.js are a hand translation. The walk,
the hash, the clock and the conversions are held to the C++ by
demo/tools/crosscheck/, which verify.sh also runs; the frame logic was
compared with the plugin once, through lntest (see AGENTS.md), and nothing
repeats that. When you change one of those, change it there too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there.
SHADERS = [
    ("COMMON", "kCommon"),
    ("QUAD_VERTEX", "kQuadVertex"),
    ("SAMPLE_FRAGMENT", "kSampleFragment"),
    ("COMPOSITE_FRAGMENT", "kCompositeFragment"),
]

# How the pieces are joined, on each side. Each entry is (file, text that must
# appear in it verbatim). The C++ lines are the plugin's assembly; the JS lines
# are plugin.js's copy of it. A change to either is a change to what runs.
ASSEMBLY = [
    ("source/Shaders.cpp",
     "std::string Assemble( const char* piece )\n{\n\tstd::string s = kVersion;\n\ts += kCommon;\n\ts += piece;\n\treturn s;\n}"),
    ("source/Langton.cpp", "const std::string quadVertex = std::string( kVersion ) + kQuadVertex;"),
    ("source/Langton.cpp", '{ &sampleShader, Assemble( kSampleFragment ), "sample" },'),
    ("source/Langton.cpp", '{ &compositeShader, Assemble( kCompositeFragment ), "composite" },'),
    ("demo/plugin.js", "const vertex = S.VERSION + S.QUAD_VERTEX;"),
    ("demo/plugin.js", "const assemble = (piece) => S.VERSION + S.COMMON + piece;"),
    ("demo/plugin.js", "sample: new Program(gl, vertex, assemble(S.SAMPLE_FRAGMENT), 'sample'),"),
    ("demo/plugin.js", "composite: new Program(gl, vertex, assemble(S.COMPOSITE_FRAGMENT), 'composite'),"),
]

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def cpp_version(source):
    match = re.search(r'const char\* const kVersion = "(.*?)";', source)
    return None if match is None else match.group(1)


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def cpp_pieces(source):
    """Every raw-string piece Shaders.cpp defines, by symbol."""
    return re.findall(r'const char\* const (k\w+) = R"\(', source)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def write(cpp):
    out = [HEADER]
    out.append(f"export const VERSION = '{cpp_version(cpp)}';\n")
    for name, symbol in SHADERS:
        body = from_cpp(cpp, symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        out.append(f"\n// {symbol}, source/Shaders.cpp\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(cpp, js):
    problems = 0
    version_cpp = cpp_version(cpp)
    version_js = re.search(r"^export const VERSION = '(.*?)';$", js, re.M)
    if version_cpp is None or version_js is None or version_cpp != version_js.group(1):
        print("FAIL  VERSION does not match kVersion")
        problems += 1
    else:
        print(f"ok    {'VERSION':<20} matches kVersion")

    listed = {symbol for _, symbol in SHADERS}
    for symbol in cpp_pieces(cpp):
        if symbol not in listed:
            print(f"FAIL  source/Shaders.cpp defines {symbol}, which this check does not list -- "
                  "add it to SHADERS (and to the page, or say on the page why not)")
            problems += 1

    for name, symbol in SHADERS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    for path, text in ASSEMBLY:
        try:
            body = read(*path.split("/"))
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    assembly: {path} still says {text.splitlines()[0][:60]}")
        else:
            print(f"FAIL  assembly: {path} no longer says: {text.splitlines()[0]}")
            problems += 1
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    if "--write" in argv:
        return write(cpp)
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(cpp, js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted copy "
              "(never edit shaders.js by hand); an assembly or unlisted-piece failure needs a reader")
        return 1
    print(f"all {len(SHADERS) + 1} shader pieces and their assembly are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
