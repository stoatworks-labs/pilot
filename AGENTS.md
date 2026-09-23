# AGENTS.md — bringing an LLM up to speed on pilot

Orientation for an AI assistant (or a new human) picking this up cold. `CLAUDE.md`
holds the short command reference; this file is the *why*: the idea, the traps that
actually bit, what is verified and what is assumed, and what is not done.

---

## 1. The one idea

**The address order is the whole plugin.**

The ZX Spectrum's display file is 6144 bytes of bitmap followed by 768 bytes of
attributes, and the bitmap is not addressed linearly. The byte holding pixel
(px, py) is at

    third*2048 + line*256 + charrow*32 + xbyte

where `third = py >> 6`, `charrow = (py >> 3) & 7` and `line = py & 7`. The row
number is taken apart into three fields and put back together most-significant
first, which is a transposition — and walking the addresses in order therefore
walks the screen in an eight-line interleave, three times.

Everything the effect does is that fact, plus two consequences of where the bytes
sit:

- The attributes come **after** the bitmap, so colour arrives in one block over the
  last 11% of the load and the picture is monochrome until then. Nothing schedules
  that.
- The **border** is the loading signal, not decoration. The ULA was told the
  current half-cycle's state and the beam painted it as it scanned, so the
  transitions land as horizontal bands and there are `baud / frameHz` pairs of them
  down the picture.

If you change nothing else, do not change the address order casually. It exists
twice on purpose — `zx::ScreenIndex` states the bit layout, `zx::ScreenOrder` walks
thirds, lines and character rows with no arithmetic on the row number at all — and
`pttest --agree` asserts the two agree for all 6144 addresses, bitwise. A third
copy lives in the compose shader, and `pttest --reveal` binds it to the other two
by rendering and comparing 49,152 pixels against the table.

## 2. The shape of it

```
source/Spectrum.{h,cpp}   the address order (twice), the palette. No GL, no FFGL.
source/Loader.{h,cpp}     the tape: progress -> bytes, block failures, border phase.
source/Machines.{h,cpp}   four loaders as a table: baud, border style, colours.
source/Controls.h         0..1 host parameters to physical units.
source/Font.{h,cpp}       graticule's 5x7 glyphs; one string is drawn from them.
source/Shaders.h          the three passes.
source/shaders/Raster.cpp   the clip, box-filtered onto 256x192.
source/shaders/Attr.cpp     one texel per 8x8 cell: ink, paper, bright, threshold.
source/shaders/Compose.cpp  the address order, the reveal, the border, the message.
source/Pilot.{h,cpp}      the plugin: parameters, the clock, the three passes.
source/PassBuffer.*       FFGLFBO with the SDK's leaked colour texture fixed.
tools/pttest/             the offline harness. Twelve check groups; ten need no GL.
tools/sweep.py            no control is silently dead.
tools/verify.sh           all of it, from a fresh universal build.
```

**Almost nothing is decided in the shader that could be decided on the CPU.** The
byte count, the border phase and the error state all arrive as uniforms from
`Loader.cpp`. That is not an optimisation — it is what lets ten of the twelve check
groups run with no GL context at all, so the claims about the address order, the
border period and the error rate cannot be a property of a rasteriser or of a
raster. What the shader owns is the thing only the shader can own: the address
arithmetic, per pixel.

### Decisions that look arbitrary and are not

- **`Baud` does not drive the reveal rate in Manual mode.** It sets the border's
  stripe rate and the Clip time period, and nothing else. Locking Progress to it
  would be more faithful and would make the effect useless: 6912 bytes at 1500 baud
  is 37 seconds, and nobody wants a 37-second transition they cannot scrub. At
  **Sync = Clip time** the two *are* locked — the tape runs at the baud rate — which
  is where the faithful behaviour lives.
- **The clip is squeezed into the border's inner rectangle, aspect and all.** An
  operator putting this on a layer wants all of the clip to arrive, framed, not a
  4:3 window onto the middle of it. On a 16:9 composition a Spectrum pixel is
  therefore wider than it is tall — exactly as it was on a widescreen television.
- **Ink is the darker group and paper the lighter.** The Spectrum's power-on
  attribute is black ink on white paper, and putting the dark half of each cell in
  the ink makes the monochrome phase read as a 1-bit threshold of the picture
  rather than as its negative.
- **The cell threshold is `mix( 0.5, midpoint, contrast )`.** The midpoint alone is
  right for a cell with something in it and wrong for a flat one, where every pixel
  sits exactly on the threshold and which side it falls depends on a comparison
  operator. Sliding a flat cell's threshold back to 0.5 makes a flat dark cell all
  ink and a flat bright cell all paper. Without it, flat regions came out as
  whichever colour the tie-break happened to pick.
- **The bit pattern is decided from the picture, not from whether the attribute has
  arrived.** On a real tape the bits were settled when the screen was converted;
  the attribute byte only says what the two colours are. So a pixel's ink/paper
  decision never changes as the load progresses — only its colours do.
- **A failed block holds the picture rather than blanking it.** The spec this was
  built to says "progress resets to zero", and it does — but a real Spectrum does
  not clear the screen when a load fails, it prints the report over whatever had
  arrived. So while the message is up the byte count holds at what the failed
  attempt had reached, and only when the message goes does the retry's count take
  over. Without this the picture vanishes the instant the error appears and the
  message is always over a blank screen, which is both wrong and much less useful.
  There is a one-frame jump at the end of the hold; a real machine would draw the
  retry over the old picture, which would need frame history and there is none.
- **`Border On` is not a colour, it is the absence of a border.** Off sets the
  inset to zero and the screen fills the composition. `Border Width` then has
  nothing to apply to, which is why the sweep's baseline has the border on.
- **`Bright` is Off / Auto / On, not a slider.** BRIGHT is one bit of hardware. It
  sets the default attribute's bright bit *and* biases the per-cell decision, so it
  has something to do both before and after colour arrives.
- **Only the ZX 48 row of `Machines.cpp` models a documented routine.** The 48K
  ROM's tape format is published — 2168 T-states per pilot half-cycle, 855 and 1710
  for a reset and a set bit against a 3.5 MHz clock, roughly 1500 bits per second —
  and red/cyan then yellow/blue are its own border colours. The other three are
  loaders in the same shape with plausible rates and colours chosen to be honestly
  distinct. The 128's *ROM* loader is byte-identical to the 48's, so "ZX 128" here
  is what a 128K release actually shipped: a turbo loader on Spectrum hardware.

## 3. The traps

Ordered by how much time they cost.

**Every sample time in the first `--border` check sat exactly on a half-cycle
boundary, and one in nine came back wrong.** The check asserts
`floor( x + 16 ) - floor( x ) == 16`, which is true for every real number. In
double it is true unless `x` is within the rounding error of an integer — and the
sample times were `k / 60`, so at 1500 baud each one was exactly 25k bits and
*every* sample was on a boundary. The fix is not a tolerance: the times now step by
an interval that is not a dyadic rational and does not divide any machine's bit
period, and the check asserts that the number of samples still landing within 1e-6
of a boundary is under 1% (it is 3 of 6000, all of them `t = 0`, which is the phase
origin). The plugin itself never does a difference of floors — it takes one floor
per row — so this was an artefact of the test, but a test that fails 11% of the
time is not a test.

**The stripe-count expectation was the wrong shape.** It was `baud / frameHz`, and
it passed at 1080 rows by 0.03 and at 240 rows by 0.13 — inside the allowance, but
for the wrong reason. Sampling row *centres* can only see the span between the
first and last of them, which is `(h-1)/h` of the frame, and that term is the only
place the raster enters at all. The expectation is now
`baud / frameHz * (h-1)/h` and the allowance is half a pair, derived: the count is
a difference of two floors, and one half-cycle is the whole of what a floor can
lose. Measured 29.500 against 29.972 and 29.875 — the *same* 29.500 at both
rasters, which is the point.

**`--bench` was timing a `glReadPixels` of the whole frame.** The harness's
`render()` reads the result back, because every other check needs the pixels — and
a readback is a synchronous stall of 33 MB at 4K. Timed as part of the render it
put 4K at 3.8 ms with a 15% spread between runs that had nothing to do with the
GPU; the real figure is 0.37 ms, stable to 0.005 ms over three runs of 300 frames.
`--bench` now calls `renderOnly()`, and warms up for 60 frames rather than 20.
Nothing else in the harness should skip the readback.

**`ScopedFBOBinding` restores the framebuffer and only the framebuffer** (SDK
`b1afaf9`, `FFGLScopedFBOBinding.cpp`). Every pass's `ResizeViewPort()` therefore
leaks into the pass after it, and the composite — which draws to the host's
framebuffer and so has no buffer of its own to size itself from — inherits whatever
the last pass left. Here that would be 32×24. `ProcessOpenGL` captures the host
viewport up front and restores it before the composite for that reason.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not restore.**
`FFGLFBO::Initialise` sizes its new colour texture under one of those, so
*allocating a buffer silently unbinds your input texture from the active unit*. The
symptom is the dangerous part: correct on every frame except the one that
allocates. Both `Ensure()` calls happen before anything binds a texture.

**`ffglex::FFGLFBO::Release()` leaks the colour texture.** It deletes the
framebuffer and the depth renderbuffer, then tests `depthBufferID` a second time
where it plainly meant `colorTextureID`. `PassBuffer::Destroy()` deletes it first.

**`FFGLScopedFBOBinding.h` is not in the umbrella header.** `FFGLSDK.h` includes
every other scoped binding and omits that one. Include it by hand; the symptom is
an unknown-type error on `ScopedFBOBinding` and nothing else.

**The SDK pins `barPhase` at 0 until a host moves it, and the harness was not a
host.** With `barPhase = 0` the fleet's bar recovery — `within + round(estimate -
within)` — returns a whole number of bars for ever, so `Beat` and `Bar` sync
correctly never move and `tools/sweep.py` correctly reported `Sync` as dead. The
plugin was right; the harness had simply never called `SetBeatInfo`. It does now,
per frame, at 120 bpm. Worth knowing before somebody "fixes" the plugin for it.

**A probe lands on whatever was drawn on top of it.** The loading-error message is
the only overlay here, and every check that reads a screen pixel calls
`Instance::quiet()` first. The exception is the message check itself, which
compares Message On against Message Off — that difference is exact, and it is the
only way to tell the message's ink from the picture's, since they are the same two
colours. graticule's burn-in plate taught the fleet this twice.

**The pinned sweep context is a *pair*, and one of the pixel checks moved half of
it.** `Message On` is only alive when a block has just failed, so `sweep.py` has a
CONTEXT entry pinning `(Error Rate, Progress)`. The `--pixels` message sub-check
overrode `Pilot Length` along with everything else, which is what maps Progress
onto the tape — so the failure moved out from under the probe and the check failed
at both rasters. `Pilot Length` is now left at its default in that block, and the
comment says why.

**The message can only move 0.34% of the frame, and the sweep's floor is 0.5%.**
That is not a dead control, it is a text overlay: 150×7 Spectrum pixels is 2.1% of
the screen and 1.5% of the frame, and only about a third of that box differs from
what was underneath. `sweep.py` gives it a floor of 0.1% — a third of what it does
move, a hundred times what a dead control would — with the arithmetic written out.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. So every ranged host parameter here is 0..1 and the
conversions live in `Controls.h`. Anything that is genuinely a whole number is
`FF_TYPE_OPTION`, which is exempt.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo` is
never referenced by name, so in a **STATIC** archive the linker may drop the whole
translation unit, giving a bundle that loads, exports `plugMain`, and reports that
it contains no plugins. `pilot_core` is an **OBJECT** library for that reason.

**`SetTextParameter` must return `FF_SUCCESS` for the About block.** The SDK's
`FF_INSTANTIATE_GL` pushes every declared default into a fresh instance and
destroys it on the first `FF_FAIL`, which is what the base implementation returns.
Omit the override and the plugin cannot be created in any real host while every
in-repo check still passes.

**GLSL `%` and `/` are undefined on negative operands**, and `layout`, `flat`,
`active`, `filter`, `input`, `output`, `sample`, `common`, `half` and `patch` are
reserved words. The border's parity is a `& 1` on a value that cannot go negative,
and the half-cycle local is `halfCycle`.

## 4. Would this check hold on a different rasteriser, at a different raster?

Every numeric check in the harness was put through that question deliberately,
one at a time, before this was called finished. The table is the record of that
pass: what each check measures, what tolerance it carries, and where the tolerance
comes from. **"Chosen" appears nowhere in the last column.** Where a bound is not
derivable it is stated as a distribution bound with its arithmetic, and where a
check is raster-sensitive it is run at two rasters — 1920×1080 and 640×480, the
second small enough to resemble a CI runner.

Five things the pass changed are in **The traps** above: the boundary-sampled
border check, the wrongly-shaped stripe-count expectation, the pixel border check's
missing float-ambiguity window, the sweep's impossible floor for `Message On`, and
`--bench` timing a whole-frame readback as though it were render cost.

| # | Check | What it measures | Tolerance | Where the tolerance comes from | GL? | Raster-sensitive? |
|---|---|---|---|---|---|---|
| 1 | `--agree` loop size | the nested loop enumerates 6144 addresses | none | integer equality | no | no |
| 2 | `--agree` mismatches | bit layout == nested loop, all 6144 | **none** | both sides are integer arithmetic; a difference of one is a transposition | no | no |
| 3 | `--agree` permutation | every address hit exactly once | none | integer count | no | no |
| 4 | `--agree` attributes | the 768 attribute addresses are 6144..6911 | none | integer range | no | no |
| 5 | `--order` revealed set | 74 byte counts × 49,152 pixels against the table | **none** | set equality on integers | no | no |
| 6 | `--order` byte atomicity | a byte's eight pixels reveal together | none | boolean over integers | no | no |
| 7 | `--thirds` thirds | each third owns its own 2048 addresses | none | integer range | no | no |
| 8 | `--thirds` interleave | row 0 < row 8 < ... < row 56 | none | strict integer inequality | no | no |
| 9 | `--thirds` not-a-wipe | row 1 is 256 bytes after row 0, not 32 | none | integer equality | no | no |
| 10 | `--attributes` first | the first attribute byte is 6144 | none | integer equality | no | no |
| 11 | `--attributes` leaks | no attribute revealed at or below 6144 | none | integer comparison over all 768 × 6145 | no | no |
| 12 | `--attributes` complete | all 768 present at 6912 | none | integer count | no | no |
| 13 | `--attributes` fraction | 6144/6912 = 0.888889 | 1e-6 | printing a rational constant, not a measurement | no | no |
| 14 | `--border` vacuity guard | samples on a half-cycle boundary | < 1% of samples | **not** a measurement tolerance: it exists so line 15 is not vacuous. Actual 3/6000, all `t = 0` | no | no |
| 15 | `--border` byte period | 8 bit periods advance the stripe by 16 half-cycles | **none** | integer equality after the boundary samples are excluded | no | no |
| 16 | `--border` continuous | the same, before the floor | 1e-6 | double rounding over a bit position reaching 1e8. Measured worst 2.9e-11 — five orders of headroom | no | no |
| 17 | `--border` stripe count | pairs down the frame vs `baud/frameHz · (h-1)/h` | 0.5 pair | the count is a difference of two floors; one half-cycle is all a floor can lose | no | **yes — run at 1080 and 240 rows** |
| 18 | `--border` two rasters | the 1080 and 240 counts against each other | 1.0 + `baud/frameHz`/240 pairs | one half-cycle each, plus the 1/240 of a frame the coarser one cannot see. Measured 0.000 | no | **yes — that is the check** |
| 19 | `--border` flash period | the flash advances one byte per byte period | **none** | integer equality, boundary samples excluded as 14 | no | no |
| 20 | `--border` flash colour | never black | none | integer inequality | no | no |
| 21 | `--border` flash spread | ≥ 5 of 7 colours over 2000 bytes | 5 of 7 | distribution bound: a uniform draw over 7 misses all of 3 with probability (4/7)^2000 | no | no |
| 22 | `--error` mean | blocks before a failure vs 1/r, 5 rates | 4σ, σ = √(1−r)/(r√N), N = 20000 | the geometric distribution's own standard error. Derived, not widened: measured 50.398 vs 50.000 at r=0.02 against a 1.400 band | no | no |
| 23 | `--error` cap | no draw hits the 200,000 cap | none | integer count | no | no |
| 24 | `--error` purity | the draw is a function of (seed, attempt, block) | none | bitwise equality over 500 repeats | no | no |
| 25 | `--error` ends | rate 0 never fails, rate 1 always does | none | boolean over 1000 | no | no |
| 26 | `--error` independence | attempt 1 is a different stream from attempt 0 | < 1500 of 2000 agree | two independent Bernoulli(0.3) streams agree 0.3²+0.7² = 58%, so ~1160; 1500 is 75%, and a shared stream would be 2000. Measured 1182 | no | no |
| 27 | `--message` pinned pair | a block has failed at (0.30, 0.54) | none | boolean on the model | no | no |
| 28 | `--message` hold | the report shows for one block of tape, no longer | none | float comparison against the constant itself | no | no |
| 29 | `--message` clean | no message at Error Rate 0 | none | boolean | no | no |
| 30 | `--clock` six days | 8 bit periods still advance the stripe by 16 at t = 5.2e5 s | **none** | integer equality; the arithmetic is double throughout | no | no |
| 31 | `--clock` negative control | the same sum in a **float** is wrong | must fail ≥ 1 of 500 | the trap stated as a measurement. Measured 500 of 500 | no | no |
| 32 | `--clock` parity | reducing the phase mod 2 keeps the stripe's parity | **none** | integer parity over 2000 samples | no | no |
| 33 | `--names` plugin name | `SW Pilot` fits the FFGL `char[16]` field | none | string length; the field is not null-terminated, so the host truncates silently | no | no |
| 34 | `--names` parameters | ≤ 16 characters, all unique | none | string comparison | no | no |
| 35 | `--names` count | the host is told about all 18 | none | integer equality | no | no |
| 36 | `--negative` ×10 | each broken model raises ≥ 1 failure | none | a check that cannot fail is not a check. Perturbations: three address orders, a 1% and a 0.1% baud error, a 15% and a 3% rate error | no | no |
| 37 | `--reveal` rendered set | 49,152 probes × 6 byte counts against the table | **none** — exact 8-bit RGB | a black input makes every cell flat and dark, so the frame is a binary readout of the reveal mask: ink (0,0,0) or paper (215,215,215) | **yes** | **yes — run at 1920×1080 and 640×480** |
| 38 | `--reveal` probe validity | every probe lands on the Spectrum pixel it was aimed at | none | the shader's own mapping, inverted in float. Not assumed — asserted | **yes** | **yes** |
| 39 | `--reveal` validator fires | at 256×192 the grid cannot be resolved | must report > 0 | the negative control for line 38. Measured 14,160 of 49,152 unresolved | **yes** | **yes — that is the check** |
| 40 | `--pixels` byte count | 6143 bytes in, from the plugin's own state | none | integer equality | **yes** | no |
| 41 | `--pixels` monochrome | before 6144 every screen pixel is the default ink or paper | **none** — exact 8-bit | the two colours are exact UNORM round-trips: 215/255 × 255 = 215.000 | **yes** | **yes — two rasters** |
| 42 | `--pixels` quadrants | red is (255,0,0), blue (0,0,255), black (0,0,0), white (255,255,255) | **none** — exact 8-bit | flat quadrants, probed at their centres, a long way from any boundary | **yes** | **yes — two rasters** |
| 43 | `--pixels` probe validity | the probe lands where aimed | none | as line 38 | **yes** | **yes** |
| 44 | `--pixels` border rows | every border row is the stripe the model predicts | **none** on colour; 1e-5 half-cycles of **undecidability** | the CPU and GPU evaluate the same sum in float from different operand orders; a span of 60 half-cycles × a float ulp of 6e-8 is 3.6e-6, so 1e-5 is ~3×. Four orders tighter than the half-cycle it would take to get a stripe wrong. Rows inside it are counted, not ignored — measured 0 at both rasters | **yes** | **yes — 1080 and 480 rows** |
| 45 | `--pixels` stripe edges | edges down the frame vs `2·baud/frameHz·(h-1)/h` | 1 edge | one floor at each end of the sampled span. Measured 59 against 59.94 and 59.88 — the *same* 59 at both rasters | **yes** | **yes — that is the check** |
| 46 | `--pixels` message box | Message On changes nothing outside the box | none | exact byte comparison of two renders | **yes** | **yes — two rasters** |
| 47 | `--pixels` message lit | ...and does change something inside it | > 0 probes | integer count | **yes** | **yes** |
| 48 | `sweep.py` liveness | each control moves > 0.5% of pixels | 0.5% | the fleet's figure, unchanged | **yes** | mildly — run at 960×540 |
| 49 | `sweep.py` Message On | ...except this one, at 0.1% | 0.1% | 150×7 of 256×192 is 2.1% of the screen and 1.5% of the frame; about a third of the box differs from what was underneath. A third of the 0.34% it actually moves, a hundred times a dead control | **yes** | mildly |
| 50 | `sweep.py` per-pixel | a channel differs by more than 2 counts | 2 of 255 | an 8-bit quantisation guard, inherited from the fleet's sweeps | **yes** | no |
| 51 | `verify.sh` shaders | exactly 4 shaders extracted, all compile | none | the count is asserted, not counted up to — a check that silently looks at nothing is worse than no check | no | no |
| 52 | `--pixels` Mix 0 | two renders at Mix 0 with everything else different | **none** — byte-identical | `mix( clip, col, 0 )` is `clip*1 + col*0`, exact in GLSL. An operator who winds Mix down gets their clip back, not something a rounding away from it | **yes** | **yes — two rasters** |
| 53 | `--bench` | ms/frame at three sizes | **not pass/fail** | there is no threshold worth asserting on somebody else's GPU. It is recorded so "it feels slower" becomes a comparison. `glFinish` on both sides and no readback; three runs of 300 frames agree to 0.005 ms | **yes** | yes, by construction |

Two things the table does not contain, and the absence is deliberate:

- **No check asserts `== 0` on an asymptote.** The only quantity here that
  approaches a limit is the error rate's mean, and it is bounded by its own
  standard error rather than by equality.
- **No check assumes two floating-point clock origins agree.** Everything temporal
  is expressed as a *difference* over one engine step (`8 / baud` seconds, one
  frame's scan, one block of tape), and `--clock` asserts that holds six days into
  a host's clock while demonstrating that the same sum in a float does not.

## 5. What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (M4 Max, macOS 26.4, GL 4.1 Metal
90.5):**

- **The address order is right, twice.** 6144 addresses, two independent
  derivations, zero mismatches, bitwise. The order is a permutation: every address
  is hit exactly once.
- **The rendered frame is the address order.** 49,152 probes × 6 byte counts × 2
  rasters = 589,824 exact 8-bit comparisons through the shipping shader, zero
  wrong, zero unresolved. Every probe validated against the shader's own inverted
  mapping rather than assumed.
- **No colour before 6144 bytes**, asserted over all 768 cells × every byte count
  up to 6144, and every cell coloured by 6912.
- **The border period is the byte period** on all four machines: 16 half-cycles per
  byte, exactly, and the same stripe count at 1080 rows as at 240.
- **The error rate is 1/r**, over 20,000 seeded draws at five rates, inside four
  standard errors of the geometric distribution's own mean every time.
- **Every one of the 10 model checks fails when the model is broken** — ten
  deliberate perturbations, ten detections.
- **All 14 controls move the picture** (`tools/sweep.py`), and **Mix 0 is a
  byte-exact bypass** at both rasters whatever else is set.
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`, and `oxbow probe` reports the name
  `SW Pilot`, the id `PT01` and an effect with 18 parameters in four groups.
  Re-probed 2026-09-23 after registration: **19** parameters — the About block
  now has five entries (About text, User guide, Project page, Source on GitHub,
  Support the work).
- **CI has run on GitHub, on a second rasteriser.** `ci.yml` on GitHub's macOS
  runner, which has no GPU, so the harness fell back to Apple's software
  renderer. The ten model checks (`--agree --order --thirds --attributes --border
  --error --message --clock --names --negative`), the two rasterising checks
  (`--reveal`, `--pixels`) and the control sweep at 320×180 all passed there.
- **The Windows x64 DLL has been compiled**, with MSVC, by `release.yml` on
  GitHub.
- **The render cost**, by `pttest --bench` (240 frames after a 60-frame warm-up,
  `glFinish` on both sides — without which this times how fast the driver accepts
  commands rather than how fast the GPU runs them — and no readback):

  | | ms/frame | % of a 60 fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.087 | 0.5% |
  | 1920×1080 | 0.208 | 1.2% |
  | 3840×2160 | 0.362 | 2.2% |

  The raster and attribute passes are fixed-size (256×192 and 32×24) whatever the
  composition is, so the growth is the compose pass and the box filter's tap count
  alone — which is why 4K costs four times 720p rather than nine. Three runs agree
  to within 0.006 ms at 1080p and 4K. The 720p figure is worth 0.02 ms less on the
  first run of a fresh process than on the second, because at a tenth of a
  millisecond a frame it is the one size where process start-up still shows.

**Assumed, or not yet done:**

- **Never loaded into Resolume on macOS**, and never installed into Arena there.
  On Windows, a CI build passed Resolume Arena 7.27.1's gate on llvmpipe on 2026-09-23, 9 of 9 (see README Status). How the parameters *present* — four groups in the inspector, two eight-entry colour
  dropdowns, Arena's premultiplied textures, its real texture sizes — is exactly
  what the offline harness cannot tell you, because it supplies its own textures.
- **The universal build has never run on Intel**, only been `lipo`-verified.
- **The four machines are four looks.** Only ZX 48 models a documented routine (see
  §2). Nothing here has been compared against a real tape, an emulator, or a
  capture of one.
- **The Clip time period is derived, not measured.** `6912 × 8 / baud`, divided by
  the non-pilot fraction. Whether 37 seconds is a useful default for an operator is
  a judgement, not a measurement.
- **`Bright = Auto`'s crossover is half way between the two hardware levels**
  (0.921 on the brightest channel in the cell). That is arithmetic; whether it is
  the right *aesthetic* call on real footage has not been checked.
- **No OpenFX port, no browser demo, no factory presets.** None of them are
  started. `pttest --pipe` (raw RGBA frames in and out, the fleet's format, with
  a `frame Name value` cue sheet) was added on 2026-09-23 to film the video; it
  is a renderer, asserts nothing, and no check runs through it.
- **Render cost figures are macOS-only.** Nothing has timed the Windows build.

## 6. Open questions

- **Should `Progress` be locked to `Baud` by default?** It is not, and §2 says why.
  The counter-argument is that an operator who picks `ZX 48` and expects a 1982
  tape gets a scrub bar instead, and only discovers the honest behaviour by finding
  `Sync = Clip time`. A second control — "Rate follows Baud" — would settle it, at
  the cost of a fifteenth parameter.
- **Is one attribute pass enough?** A cell's two colours are recomputed every
  frame from the current clip. On moving footage a cell's palette therefore
  *changes* during the load, which a real tape could never do: the screen was
  converted once, before it was recorded. Freezing the attribute pass at the
  instant the load starts would be more faithful and would need a third buffer and
  a notion of when a load began — which is state, and there is none here.
- **The C64 turbo flash colour is a hash of the byte index.** A real turbo loader's
  border carried the *data*, so the flicker was the file. Deriving the colour from
  the byte actually being loaded is possible — the byte is eight pixels of the
  picture, which the shader already has — and would be a genuinely lovely detail.
  It was left out because the border is currently a pure function of time with no
  dependence on the raster or the attribute buffers, and that is what makes
  `--border` a CPU check.
- **What should happen at `Progress = 1` with `Error Rate` up?** Currently the last
  block can fail and the picture is left part-loaded for ever. Arguably the tape
  should be allowed to finish once, which would need the failure draw to know it
  was on the last block.

## 7. Conventions

- MIT, copyright Stoatworks Labs. Public at `github.com/stoatworks-labs/pilot`,
  created 2026-09-23; released at v0.1.0 on 2026-09-23.
- Registered in the website's `projects.json` (status beta, guide true) and in
  stoatworks-backend's `sync-about.py` TARGETS, `names.json`, `visibility.json`
  and `derived.json`. `source/StoatworksAbout.h` is GENERATED by `sync-about.py`
  and `ATTRIBUTIONS.md` by `sync-attributions.py` — never edit either by hand.
- User guide: `docs/USER-GUIDE.md` is the only copy anyone edits; the website's
  `build_guides.py` renders it to `docs/USER-GUIDE.pdf` and to
  https://stoatworks-labs.com/software/pilot/guide/.
- Display name `SW Pilot`, FFGL id `PT01`, bundle id `com.stoatworks.ffgl.pilot`,
  version `0.1.0` in both `CMakeLists.txt` and `source/StoatworksAbout.h`.
- Standard AI disclaimer in the README, and it says what the harness proves.

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes). The two files worth
reading before touching any FFGL plugin here are `tinsel/AGENTS.md` ("The traps")
and `graticule/CLAUDE.md` ("Notes").
