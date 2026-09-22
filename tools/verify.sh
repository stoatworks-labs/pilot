#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each check answers a question none of the others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out
#   build         a fresh universal Release build
#   model         the ten checks that need no GL context at all: the two address
#                 derivations against each other, the revealed set against the
#                 independently written table, the interleave, the attribute
#                 block, the border period at two rasters, the error rate
#                 against 1/r, the pinned sweep context, the clock, the names,
#                 and the negative controls that prove all of the above can fail
#   render        the two checks that rasterise, each run at 1920x1080 AND at
#                 640x480, because a thresholded position measurement quantises
#                 to whole cells and a check that only runs at one raster cannot
#                 tell you that
#   sweep         does every control change the picture
#   bench         the render cost, for the record -- not pass/fail, because
#                 there is no threshold worth asserting on somebody else's GPU
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain
#   lipo          is the macOS build really universal, or did CMake latch the
#                 architecture list before -DCMAKE_OSX_ARCHITECTURES arrived
#                 and report success anyway
#   plist         does CFBundleExecutable name the binary that is actually on
#                 disk -- if it does not, codesign reports "code object is not
#                 signed at all" about a *nested* object and mentions neither
#                 the plist nor the cause
#   codesign      the exact command the release job runs, against a copy
#   oxbow         does a host see the right name, id and type
#
# The last five are release-job work done locally on purpose. A check that only
# runs in CI, after a tag, is a check that will catch you after the tag -- and
# the fix for a bad tag is to re-point it, which strands the release unsigned
# for ever unless the autosign state file is edited by hand.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/shaders/Vertex.cpp",
	"source/shaders/Raster.cpp",
	"source/shaders/Attr.cpp",
	"source/shaders/Compose.cpp",
]

# A shader may be several adjacent raw strings (MSVC caps one literal at about
# 16 KB), so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	# Four shaders, and the number is asserted rather than counted up to. No
	# shaders at all is a FAILURE, not a pass: it means the extraction has lost
	# track of where this repo keeps its GLSL, and a check that silently looks
	# at nothing is worse than no check.
	if [ "$n" -ne 4 ]; then
		printf '   extracted %d shaders, expected 4 -- the extraction has gone stale\n' "$n"
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# A FRESH universal Release build. Fresh because the architecture list is
# latched when the first target is created, so a tree configured earlier as
# arm64-only stays arm64-only however many times it is rebuilt.
#---------------------------------------------------------------------------
step "build (fresh, universal, Release)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
	&& cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "configures and builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

#---------------------------------------------------------------------------
# The model. None of this opens a GL context, so none of it can be a property
# of a rasteriser or of a raster.
#---------------------------------------------------------------------------
step "model (no GL context)"
for t in agree order thirds attributes border error message clock names negative; do
	if "$BUILD/pttest" --$t >/dev/null 2>&1; then pass "pttest --$t"; else fail "pttest --$t"; fi
done

#---------------------------------------------------------------------------
# The two that rasterise. Both run at 1920x1080 and at 640x480 internally.
#---------------------------------------------------------------------------
step "render (1920x1080 and 640x480)"
for t in reveal pixels; do
	if "$BUILD/pttest" --$t >/dev/null 2>&1; then pass "pttest --$t"; else fail "pttest --$t"; fi
done

step "sweep"
if python3 tools/sweep.py >/tmp/pilot-sweep.txt 2>&1; then
	tail -1 /tmp/pilot-sweep.txt | sed 's/^/   /'
	pass "no dead controls"
else
	fail "tools/sweep.py reports a dead control -- see /tmp/pilot-sweep.txt"
	tail -5 /tmp/pilot-sweep.txt
fi

step "bench (for the record, not pass/fail)"
"$BUILD/pttest" --bench 2>&1 | sed -n '3,8p'

BUNDLE="$BUILD/Pilot.bundle"
BIN="$BUNDLE/Contents/MacOS/Pilot"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure. It
	# is output-size dependent, so it fires on the bigger binary first and looks
	# intermittent. Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ "$ident" = "com.stoatworks.ffgl.pilot" ]; then
		pass "CFBundleIdentifier is com.stoatworks.ffgl.pilot"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Pilot.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		out=$("$OXBOW" probe "$BUNDLE" 2>&1)
		printf '%s\n' "$out" | sed 's/^/     /'
		case "$out" in
			*"SW Pilot"*) pass "the host sees the name 'SW Pilot'" ;;
			*) fail "the host does not see 'SW Pilot'" ;;
		esac
		case "$out" in
			*PT01*) pass "the host sees the id PT01" ;;
			*) fail "the host does not see the id PT01" ;;
		esac
		case "$out" in
			*[Ee]ffect*) pass "the host sees an effect" ;;
			*) fail "the host does not see an effect" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
