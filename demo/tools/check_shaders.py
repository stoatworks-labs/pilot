"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds a second copy of every GLSL string under
`source/shaders/`, and two copies drift -- quietly, because a tape loader that
paints a *plausible* interleave looks exactly like one that paints the right
one. The compose shader carries the third copy of the Spectrum's address order,
the one that actually paints the screen; `pttest --reveal` binds the C++ copy of
it to the order table, and this binds the page's copy to the C++. Nothing else
does: `pttest` has never heard of this page, and verify.sh's glslc step
compiles the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment updated on one side and not the other is exactly
the drift worth catching, because the comments in these shaders carry the
reasoning (why the border is a mask and not a modulo, why the threshold is
`mix( 0.5, midpoint, contrast )`, where the only two flips in the plugin are).

The one transformation is a decode, not a normalisation. Several comments quote
an identifier in backticks, and a backtick cannot appear raw inside a
JavaScript template literal, so `plugin.js` escapes it as \\`. This unescapes
that and *rejects any other backslash on the JS side*; there is none in the C++
shader text, so a second escape could only be somebody hiding a difference.

It also checks the loading-error message, which is not a shader but is drawn
from data the plugin owns: the string, its position, and every 5x7 glyph the
page carries against the same row of `Font.cpp`.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half: `Loader.cpp`, `Machines.cpp`,
`Controls.h`, the palette and `effectiveProgress()` in plugin.js are a hand
translation, and only a reader can tell whether they still agree. Change one of
those and change it here too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
SHADERS = [
    ("VERTEX", "source/shaders/Vertex.cpp", "kVertex"),
    ("RASTER", "source/shaders/Raster.cpp", "kRasterFragment"),
    ("ATTR", "source/shaders/Attr.cpp", "kAttrFragment"),
    ("COMPOSE", "source/shaders/Compose.cpp", "kComposeFragment"),
]


def check_message(js):
    """The message string, its origin, and every glyph the page carries."""
    problems = 0
    pilot = open(os.path.join(REPO, "source", "Pilot.cpp")).read()
    font = open(os.path.join(REPO, "source", "Font.cpp")).read()

    for what, cpp_pattern, js_pattern in [
        ("message text", r'kErrorMessage = "([^"]*)";', r"const kErrorMessage = '([^']*)';"),
        ("message x", r"kMessageX = (\d+);", r"const kMessageX = (\d+);"),
        ("message y", r"kMessageY = (\d+);", r"const kMessageY = (\d+);"),
    ]:
        a = re.search(cpp_pattern, pilot)
        b = re.search(js_pattern, js)
        if a is None or b is None or a.group(1) != b.group(1):
            print(f"FAIL  {what}: C++ {a and a.group(1)!r}, js {b and b.group(1)!r}")
            problems += 1
        else:
            print(f"ok    {what:<14} {a.group(1)!r}")

    table = {int(code): rows for rows, code in
             re.findall(r'\{ ("[.#]{5}"(?:, "[.#]{5}"){6}) \}, // (\d+) ', font)}
    carried = dict(re.findall(r"^  (\d+): \[([^\]]*)\],", js, re.M))
    message = re.search(r'kErrorMessage = "([^"]*)";', pilot)
    needed = sorted({ord(ch) for ch in message.group(1)}) if message else []
    for code in needed:
        if str(code) not in carried:
            print(f"FAIL  glyph {code} ({chr(code)!r}) is in the message and not on the page")
            problems += 1
            continue
        if carried[str(code)].replace("'", '"') != table.get(code):
            print(f"FAIL  glyph {code} ({chr(code)!r}) differs from Font.cpp")
            problems += 1
    if len(carried) != len(needed):
        print(f"FAIL  the page carries {len(carried)} glyphs and the message needs {len(needed)}")
        problems += 1
    if not problems:
        print(f"ok    glyphs         all {len(needed)} the message uses match Font.cpp row for row")
    return problems


def from_cpp(source, symbol):
    match = re.search(r'(?:static )?const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    if "${" in body:
        return None, "template substitution inside a shader literal"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()
    cpp_cache = {}

    problems = 0
    for name, path, symbol in SHADERS:
        if path not in cpp_cache:
            with open(os.path.join(REPO, path)) as handle:
                cpp_cache[path] = handle.read()
        cpp_text = from_cpp(cpp_cache[path], symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<8} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")
        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    problems += check_message(js)

    print()
    if problems:
        print(f"{problems} problem(s) -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders, the message and its glyphs are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
