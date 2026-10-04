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
#   openfx copy   the OpenFX build's CPU copy of the passes (Render.cpp) against
#                 the GPU, pixel by pixel, every difference explained or a
#                 failure -- and its transition against the GPU's own frames
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
#   openfx        the OpenFX bundle: plist, entry point, both slices, the
#                 ad-hoc sign, and an OFX host (ofxprobe) loading it, listing
#                 the Transition context and rendering a frame that is not its
#                 input -- and, given a probe that can host a Transition
#                 (OFXPROBE=...), rendering one: Cut against Render.cpp, and
#                 Fade's ends, ramps and middle
#
# The last five are release-job work done locally on purpose. A check that only
# runs in CI, after a tag, is a check that will catch you after the tag -- and
# the fix for a bad tag is to re-point it, which strands the release unsigned
# for ever unless the autosign state file is edited by hand.
#
set -uo pipefail

cd "$(dirname "$0")/.."

# resolume-ofx-bridge, for ofxprobe. It sits beside this repo's checkout --
# and from a git worktree `..` is the worktrees folder, not Projects/resolume,
# so the main checkout is found through git's common dir as well.
# PILOT_BRIDGE overrides both, and OFXPROBE the probe itself.
BRIDGE="${PILOT_BRIDGE:-}"
if [ -z "$BRIDGE" ]; then
	for candidate in "../resolume-ofx-bridge" \
	                 "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)")/../resolume-ofx-bridge"; do
		if [ -d "$candidate/build" ]; then
			BRIDGE="$candidate"
			break
		fi
	done
fi
BRIDGE="${BRIDGE:-../resolume-ofx-bridge}"

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
# The browser demo's copy of the same GLSL.
#
# demo/plugin.js cannot include a C++ file, so it carries its own copy of every
# shader, and two copies drift quietly: the plugin keeps working, the page keeps
# working, and they stop being the same effect. This compares them character for
# character. It says nothing about the page's PORT of the CPU half; only a
# reader checks that.
#---------------------------------------------------------------------------
step "demo: the browser copy of the shaders"
if python3 demo/tools/check_shaders.py >/tmp/pilot-demo-shaders.log 2>&1; then
	pass "$( tail -1 /tmp/pilot-demo-shaders.log )"
else
	fail "the demo's shaders have drifted -- see /tmp/pilot-demo-shaders.log"
	tail -12 /tmp/pilot-demo-shaders.log
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

#---------------------------------------------------------------------------
# The OpenFX build renders with Render.cpp, a CPU copy of the three passes.
# --cpu holds it to the GPU at 1920x1080, 640x480 and on ofxprobe's own ramp;
# --transition holds the OpenFX transition to what the GPU's frames imply.
#---------------------------------------------------------------------------
step "openfx copy (Render.cpp against the GPU)"
for t in cpu transition; do
	if "$BUILD/pttest" --$t >/tmp/pilot-$t.log 2>&1; then
		pass "pttest --$t"
	else
		fail "pttest --$t -- see /tmp/pilot-$t.log"
		grep FAIL /tmp/pilot-$t.log | head -5
	fi
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
"$BUILD/pttest" --cpu-bench 2>&1 | sed -n '3,8p'

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

#---------------------------------------------------------------------------
# The OpenFX bundle. Release-job work done locally, for the reason at the top.
#
# cmake/InfoOFX.plist.in is copied from repo to repo, and a CFBundleExecutable
# naming the wrong binary passes the build, lipo, nm and a render -- and fails
# only in codesign, after the tag, with a message about a nested object.
#---------------------------------------------------------------------------
OFX="$BUILD/Pilot.ofx.bundle"
OFXBIN="$OFX/Contents/MacOS/Pilot.ofx"

if [ "$(uname)" = "Darwin" ]; then
	step "openfx"
	if [ ! -f "$OFXBIN" ]; then
		fail "no OpenFX bundle at $OFX (built with -DBUILD_OFX=OFF?)"
	else
		exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$OFX/Contents/Info.plist" 2>/dev/null)
		if [ -n "$exe" ] && [ -f "$OFX/Contents/MacOS/$exe" ]; then
			pass "CFBundleExecutable ($exe) is on disk"
		else
			fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
		fi
		ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$OFX/Contents/Info.plist" 2>/dev/null)
		if [ "$ident" = "com.stoatworks.pilot.ofx" ]; then
			pass "CFBundleIdentifier is com.stoatworks.pilot.ofx"
		else
			fail "CFBundleIdentifier is '$ident'"
		fi

		# Captured, not piped into grep -q: see the registration step.
		syms=$(nm -gU "$OFXBIN" 2>/dev/null)
		case "$syms" in
			*_OfxGetPlugin*) pass "exports OfxGetPlugin" ;;
			*) fail "no OfxGetPlugin -- no OpenFX host will see a plugin" ;;
		esac

		archs=$(lipo -archs "$OFXBIN" 2>/dev/null)
		case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
		case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs)" ;; esac

		tmp=$(mktemp -d)
		cp -R "$OFX" "$tmp/" 2>/dev/null
		if codesign --force --sign - --timestamp=none "$tmp/Pilot.ofx.bundle" >/dev/null 2>&1; then
			pass "ad-hoc signs (the command the release job runs)"
		else
			fail "ad-hoc signing the OpenFX bundle failed"
		fi
		rm -rf "$tmp"

		# A host loading it. ofxprobe also scans /Library/OFX/Plugins and takes
		# the FIRST bundle with a matching identifier, so an installed Pilot
		# would be what got tested; say so rather than report on the wrong one.
		OFXPROBE="${OFXPROBE:-$BRIDGE/build/ofxprobe}"
		if [ ! -x "$OFXPROBE" ]; then
			printf '   skipped: ofxprobe not built at %s\n' "$OFXPROBE"
		elif [ -e "/Library/OFX/Plugins/Pilot.ofx.bundle" ]; then
			printf '   skipped: /Library/OFX/Plugins/Pilot.ofx.bundle would shadow the build in ofxprobe\n'
		else
			listing=$("$OFXPROBE" --dir "$BUILD" 2>&1)
			case "$listing" in
				*com.stoatworks.pilot*OfxImageEffectContextTransition*) pass "a host lists it, with the Transition context" ;;
				*) fail "ofxprobe does not list com.stoatworks.pilot with a Transition context" ;;
			esac
			result=$("$OFXPROBE" --dir "$BUILD" --render com.stoatworks.pilot --size 640x360 2>&1)
			case "$result" in
				*"rendered 640x360"*) ;;
				*) fail "the OpenFX bundle does not render"; printf '%s\n' "$result" | sed 's/^/     /' ;;
			esac
			case "$result" in
				*" 0 of "*"bytes differ"*) fail "the OpenFX bundle renders its input unchanged" ;;
				*"bytes differ"*) pass "it renders ($(printf '%s\n' "$result" | grep -oE '[0-9]+ of [0-9]+ bytes differ'))" ;;
			esac

			# The TRANSITION, rendered through a host -- when the probe can
			# host one (an ofxprobe with --context; the bridge's stock probe
			# instantiates the Filter context only, so this is skipped unless
			# OFXPROBE names one that can). Small pictures, opaque, so the PPM
			# the host writes is the whole picture:
			#
			#   Cut  the raw load. Against Render.cpp in this harness
			#        (pttest --pipe --via-cpu): the incoming picture's Paper
			#        and Black frames say which addresses have arrived, and the
			#        transition is the outgoing picture through the rest --
			#        byte for byte, at two positions. The control, the host at
			#        0.3 against the harness at 0.35, must miss.
			#   Fade the default. Transition 0 is SourceFrom and 1 is SourceTo
			#        byte for byte, rendered and through isIdentity, in 8-bit
			#        and float; on a ramp it is ( 1 - s ) plain + s Cut at the
			#        remapped progress, within one level; between the ramps it
			#        is that Cut, byte for byte.
			help=$("$OFXPROBE" --help 2>&1)
			case "$help" in
				*"--context"*)
					if python3 - "$OFXPROBE" "$BUILD" >/tmp/pilot-ofx-transition.log 2>&1 <<'TRANSITION_PY'
import os, subprocess, sys, tempfile
probe, build = sys.argv[1:3]
pttest = os.path.join(build, "pttest")
W, H, FRAME = 320, 180, 13
def picture(kind):
	out = bytearray()
	for y in range(H):
		for x in range(W):
			u, v = (x + 0.5) / W, (y + 0.5) / H
			if kind == "a":
				c = ((255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (30, 30, 30))[min(7, int(u * 8))]
			else:
				d = (u - 0.5) ** 2 + (v - 0.5) ** 2
				c = (int(255 * u), int(255 * v), 200) if d > 0.06 else (240, 120, 30)
			out += bytes(c)
	return bytes(out)
tmp = tempfile.mkdtemp()
rgb = {}
for k in "ab":
	rgb[k] = picture(k)
	open(os.path.join(tmp, k + ".ppm"), "wb").write(b"P6\n%d %d\n255\n" % (W, H) + rgb[k])
def host(t, sets, depth="byte"):
	out = os.path.join(tmp, "host.ppm")
	cmd = [probe, "--no-system-dirs", "--dir", build, "--render", "com.stoatworks.pilot", "--context", "transition",
	       "--from", os.path.join(tmp, "a.ppm"), "--to", os.path.join(tmp, "b.ppm"), "--frame-rate", "25",
	       "--time", str(FRAME), "--transition", repr(t), "--depth", depth, "--out-only", out]
	for s in sets:
		cmd += ["--set", s] if s != "--identity" else [s]
	r = subprocess.run(cmd, capture_output=True, text=True)
	where = [l.split("instance from ", 1)[1].rsplit(" (", 1)[0] for l in r.stdout.splitlines() if "instance from " in l]
	if r.returncode != 0 or not where or os.path.realpath(os.path.dirname(where[0])) != os.path.realpath(build):
		print(r.stdout, r.stderr)
		sys.exit(1)
	data = open(out, "rb").read()
	return data[data.index(b"255\n") + 4:]
def harness(progress, background):
	rgba = bytearray()
	for i in range(0, len(rgb["b"]), 3):
		rgba += rgb["b"][i:i + 3] + b"\xff"
	cmd = [pttest, "--pipe", "--via-cpu", "--size", "%dx%d" % (W, H), "--fps", "25",
	       "--set", "Progress=%r" % progress, "--set", "Background=%d" % background]
	r = subprocess.run(cmd, input=bytes(rgba) * (FRAME + 1), capture_output=True)
	frame = r.stdout[FRAME * W * H * 4:]
	return bytes(frame[i] for i in range(len(frame)) if i % 4 != 3)
def cut_expected(progress):
	paper, black = harness(progress, 0), harness(progress, 1)
	out = bytearray(paper)
	for i in range(0, len(out), 3):
		if paper[i:i + 3] != black[i:i + 3]:
			out[i:i + 3] = rgb["a"][i:i + 3]
	return bytes(out)
def worst(a, b):
	return max(abs(x - y) for x, y in zip(a, b))
bad = 0
for t in (0.3, 0.6):
	same = host(t, ["ends=1"]) == cut_expected(float(t))
	print("Cut  Transition %s: %s the harness's Render.cpp" % (t, "byte-identical to" if same else "DIFFERS from"))
	bad += not same
w = worst(host(0.3, ["ends=1"]), cut_expected(0.35))
print("control: the host at 0.3 against the harness at 0.35: worst %d of 255" % w)
bad += w <= 1
for depth in ("byte", "float"):
	for t, k in ((0, "a"), (1, "b")):
		for how in ([], ["--identity"]):
			same = host(t, how, depth) == rgb[k]
			print("Fade Transition %s %-5s %-10s is %s: %s" % (t, depth, " ".join(how) or "rendered", "SourceFrom" if k == "a" else "SourceTo", "byte-identical" if same else "DIFFERS"))
			bad += not same
L = 0.15
def progress(t):
	return min(max((t - L) / (1 - 2 * L), 0.0), 1.0)
def strength(t):
	e = min(t, 1 - t)
	x = e / L
	return 1.0 if e >= L else x * x * (3 - 2 * x)
for t, k in ((0.06, "a"), (0.95, "b")):
	s = strength(t)
	got, cut = host(t, []), host(progress(t), ["ends=1"])
	w = max(abs(g - ((1 - s) * p + s * c)) for g, p, c in zip(got, rgb[k], cut))
	print("Fade Transition %s, s %.3f: worst %.2f of 255 from ( 1 - s ) plain + s Cut" % (t, s, w))
	bad += w > 1
same = host(0.5, []) == host(progress(0.5), ["ends=1"])
print("Fade Transition 0.5, between the ramps: %s Cut at %.4f" % ("byte-identical to" if same else "DIFFERS from", progress(0.5)))
bad += not same
sys.exit(1 if bad else 0)
TRANSITION_PY
					then
						pass "renders as a Transition in an OFX host: Cut byte-identical to Render.cpp (control rejected); Fade's ends byte-identical to the clips in 8-bit and float, its ramp the crossfade, its middle Cut"
					else
						sed 's/^/     /' /tmp/pilot-ofx-transition.log
						fail "the OpenFX transition rendered through $OFXPROBE disagrees -- see /tmp/pilot-ofx-transition.log"
					fi
					;;
				*) printf '   skipped: this ofxprobe cannot host a Transition (no --context); OFXPROBE=<a probe that can> to render one\n' ;;
			esac

			# Resolve's Fusion page reports no frame rate on its clips, and an
			# unguarded read throws out of the render (found in a real Resolve
			# 21.1). A probe with --quirks fusion is stricter: it leaves the
			# effect's rate out too, so under it the plugin must render, and
			# render exactly what a host reporting 24 fps gets -- the
			# fallback -- as a filter on Clip time (the border and the period
			# both run on that clock) and as a Fade transition. Skipped with a
			# probe that has no --quirks.
			case "$help" in
				*"--quirks"*)
					tmp=$(mktemp -d)
					quirk_ok=1
					python3 -c "
import sys
W, H = 160, 90
for path, k in ((sys.argv[1], 0), (sys.argv[2], 1)):
    out = bytearray()
    for y in range(H):
        for x in range(W):
            out += bytes(((x * 3 + y * k * 2) & 255, (y * 5) & 255, (200 if k else 60)))
    open(path, 'wb').write(b'P6\\n%d %d\\n255\\n' % (W, H) + bytes(out))
" "$tmp/in.ppm" "$tmp/to.ppm"
					filter_args=( --in "$tmp/in.ppm" --set sync=1 --set progress=0.1 )
					transition_args=( --context transition --from "$tmp/in.ppm" --to "$tmp/to.ppm" --transition 0.4 )
					for name in filter transition; do
						if [ "$name" = filter ]; then args=( "${filter_args[@]}" ); else args=( "${transition_args[@]}" ); fi
						"$OFXPROBE" --no-system-dirs --dir "$BUILD" --render com.stoatworks.pilot "${args[@]}" \
							--time 37 --quirks fusion --out-only "$tmp/quirk-$name.ppm" >"$tmp/quirk-$name.log" 2>&1 || quirk_ok=0
						"$OFXPROBE" --no-system-dirs --dir "$BUILD" --render com.stoatworks.pilot "${args[@]}" \
							--time 37 --frame-rate 24 --out-only "$tmp/at24-$name.ppm" >"$tmp/at24-$name.log" 2>&1 || quirk_ok=0
						if ! cmp -s "$tmp/quirk-$name.ppm" "$tmp/at24-$name.ppm"; then
							quirk_ok=0
							printf '     %s: the --quirks fusion render is not the 24 fps one\n' "$name"
							tail -5 "$tmp/quirk-$name.log" | sed 's/^/       /'
						fi
					done
					if [ "$quirk_ok" = 1 ]; then
						pass "renders under --quirks fusion (no frame rate anywhere), as a filter and a transition, exactly as at 24 fps"
					else
						fail "the OpenFX plugin does not render under --quirks fusion as it does at 24 fps"
					fi
					rm -rf "$tmp"
					;;
				*) printf '   skipped: this ofxprobe has no --quirks; OFXPROBE=<a probe that has> to render under --quirks fusion\n' ;;
			esac
		fi
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
