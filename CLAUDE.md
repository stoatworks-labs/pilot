# pilot

A ZX Spectrum tape loader as an FFGL **effect** for Resolume Arena/Avenue, and as an
OpenFX effect **and transition** for Resolve/Vegas/Nuke/Natron (`Pilot.ofx.bundle`,
CPU render, macOS universal / Win64 / Linux x86_64). C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT. Public at
github.com/stoatworks-labs/pilot, released at v0.1.0 on 2026-09-23. 19 parameters
(14 controls + a five-entry About block). User guide: `docs/USER-GUIDE.md` (the only
copy anyone edits), rendered to `docs/USER-GUIDE.pdf` and
https://stoatworks-labs.com/software/pilot/guide/ by the website's `build_guides.py`.

Read `AGENTS.md` before changing the address order, the attribute pass or anything
in `Loader.cpp`.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build`
- OpenFX plugin: `build/Pilot.ofx.bundle` (copy to `/Library/OFX/Plugins` by hand);
  `-DBUILD_OFX=OFF` skips it, `-DPILOT_BUILD_FFGL=OFF` builds it alone (no submodule,
  no GLEW, no GL — the Linux job)
- Render a frame offline: `./build/pttest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Progress=0.6" --set "Type=2" --set "Baud=0.8"`
- List parameters: `./build/pttest --list`
- Other pictures: `--flat 0` (uniform), `--quads` (four flat quadrants, for probes)
- Film: `ffmpeg ... -f rawvideo -pix_fmt rgba - | ./build/pttest --pipe --size 1920x1080 --fps 30 --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mp4`

## Verify
- Everything: `tools/verify.sh` (fresh **universal** Release build + every check, ~12 s)
- Ten check groups that need **no GL context at all**:
  - `./build/pttest --agree` — the bit layout against the nested loop, all 6144, bitwise
  - `./build/pttest --order` — the revealed set is exactly the first n addresses
  - `./build/pttest --thirds` — the interleave is present, thirds in order
  - `./build/pttest --attributes` — no colour before 6144 bytes
  - `./build/pttest --border` — the stripe period is the byte period, at two rasters
  - `./build/pttest --error` — blocks before a failure, against 1/r, 20000 draws
  - `./build/pttest --message` — the sweep's pinned Message On context still fails a block
  - `./build/pttest --clock` — the border survives a six-day host clock
  - `./build/pttest --names` — nothing the host will silently truncate
  - `./build/pttest --negative` — break the model; every check above must fail
- Two that rasterise, each at **1920x1080 and 640x480**:
  - `./build/pttest --reveal` — the rendered frame against the order table
  - `./build/pttest --pixels` — attributes, border and message
- `./build/pttest --bench` — 720p, 1080p, 4K
- The OpenFX build's renderer (`source/Render.cpp`) against the GPU:
  - `./build/pttest --cpu` — the card at 1920x1080 and 640x480 and ofxprobe's ramp,
    stage by stage (raster bytes, cells, frame); every pixel that differs by more
    than one level must be a tie (a decision on its edge, shown to be) or a raster
    rounding, or it fails
  - `PTTEST_SOFTWARE=1 ./build/pttest --cpu` — the same on Apple's software
    renderer, which is what CI's GPU-less runner gets; it needs ties the M4 does not
  - `./build/pttest --transition` — `render::Transition` against the transition the
    GPU's own Paper/Black frames imply, six positions × two Mix
  - `./build/pttest --cpu-bench` — its cost, threaded and not
  - `./build/pttest --pipe --via-cpu ...` — `--pipe` through Render.cpp instead
- `python3 tools/sweep.py` — no dead controls (`--size WxH`, `--frames N`)
- `python3 demo/tools/check_shaders.py` — the browser demo still runs the plugin's GLSL

## Notes
- **One frame's CPU decisions live in `Frame.cpp`** (`frame::Prepare`: the byte count,
  border phase and colours, inset, power-on attribute, message) and so do the
  controls' defaults (`frame::HostValues`). `Pilot.cpp` and the OpenFX plugin both
  call it. Change a uniform there, never inline in `ProcessOpenGL`.
- **The three shaders are mirrored in `Render.cpp`**, marked `//= mirrored:` on both
  sides (outside the GLSL raw strings, because `demo/plugin.js` carries those
  character for character). Edit both, then run `pttest --cpu --transition`.
- **The RGBA16F threshold is rounded toward zero** in `render::Half`, because the M4
  Max's GL does exactly that (`pttest --cpu` reads the GPU's cells back). OpenGL
  leaves it to the implementation; the check accepts one half-float step up too.
- **The address order is the plugin.** It exists twice — `zx::ScreenIndex` (the bit
  layout) and `zx::ScreenOrder` (a nested loop) — and `--agree` proves they agree.
  A third copy lives in the compose shader; `--reveal` binds it to the other two by
  rendering. Change one, run all three.
- **Everything about *where the load has got to* is computed on the CPU**, in
  `Loader.cpp`, once per frame, and handed to the shader as a byte count and a
  phase. That is what lets ten of the twelve check groups run with no GL.
- **`Baud` does not drive the reveal in Manual mode.** It sets the border's stripe
  rate and the Clip time period. Locking the reveal to it would make a 6912-byte
  load take 37 seconds and the effect useless as a transition. See AGENTS.md.
- **Time is double throughout.** Resolume's clock is milliseconds since the
  composition opened; a float resolves only ~0.03 s past 5e8. What reaches the
  shader is the border phase reduced to [0, 2), which is small at any uptime.
- **Two y-flips, and only two.** The raster and attribute buffers are stored
  bottom-up like every GL texture; the compose pass reads them with `191 - py` and
  `23 - cy` because the display file is addressed from the top. Nothing else flips.
- **GLSL `%` and `/` are undefined on negative operands.** The border's parity is a
  `& 1` on a value that cannot go negative, not a modulo.
- **Reserved GLSL words**: `layout`, `flat`, `active`, `filter`, `input`, `output`,
  `sample`, `common`, `half`, `patch`. The half-cycle local is `halfCycle`.
- **Randomness is integer hashing** (PCG output mix, `load::HashInt`), never
  `fract(sin(...))` — the error rate must not be a property of somebody's libm.
- `SetParamInfo` clamps a STANDARD default into 0..1, so every ranged parameter is
  0..1 and the conversions live in `Controls.h`. Whole numbers are OPTION.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host
  can instantiate the plugin at all.
- `pilot_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name. `pilot_model` (everything with
  no GL) is a second OBJECT library, and every final target names BOTH: an OBJECT
  library's objects do not travel through another OBJECT library.
- In `pttest`, a bare `render( ... )` outside the anonymous namespace is ambiguous
  with the `pilot::render` namespace (`using namespace pilot`). Inside it, fine.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `PT01`; the display name is `SW Pilot`.

## CI and registration
- `ci.yml` runs on GitHub's macOS runner, which has no GPU; the harness falls back
  to Apple's software renderer. The ten model checks, `--reveal`, `--pixels` and
  the sweep at 320×180 all passed there.
- `ci.yml` also runs `--cpu` and `--transition` there, checks the OpenFX bundle, and
  builds the OpenFX plugin on Windows with `PILOT_BUILD_FFGL=OFF` (no vcpkg).
- `release.yml` compiles the Windows x64 DLL with MSVC, and the OpenFX plugin for
  macOS, Windows and Linux (`linux-ofx` on AlmaLinux 8 for glibc 2.28, `linux-load`
  dlopens it on Rocky 8) as `pilot-ofx-<platform>.zip`.
- Registered in the website's `projects.json` and stoatworks-backend's sync
  scripts. `StoatworksAbout.h` is GENERATED by `sync-about.py` and
  `ATTRIBUTIONS.md` by `sync-attributions.py` — never edit either by hand.

## Not done yet
- Never loaded into Resolume on macOS, and never installed into Arena there. On Windows, a CI build passed Resolume Arena 7.27.1's gate on llvmpipe on 2026-09-23, 9 of 9 (see README Status).
- The universal build has never run on an Intel Mac.
- No factory presets.
- The OpenFX build has never been loaded into Resolve, Vegas, Nuke or Natron — only
  into a command-line test host (an extended ofxprobe), in the Filter and Transition
  contexts. The Windows and Linux OpenFX builds have never rendered a frame.
- Render cost figures are macOS-only.

## Browser demo

`demo/` is the page at **pilot-demo.stoatworks-labs.com**: the plugin's own
shaders, copied across unedited, plus a hand port of the whole CPU chain —
`Loader.cpp`, `Machines.cpp`, `Controls.h`, the palette and
`effectiveProgress()`. The kit in `demo/vendor/` is vendored from
`infrastructure/stoatworks-backend/resolume-demo/` by its `sync.sh` — fix a kit
bug THERE, never here. There is no build step; `cf-run npx wrangler deploy` from
the repo root uploads `demo/` as it stands, and the page is verified by content
rather than by status code. `AGENTS.md` §8 has what the page leaves out and why.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/pilot/pilot.YYYY-MM-DD.log
