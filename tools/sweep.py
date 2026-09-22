"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage has something to do, and report any that made no difference.

    python3 tools/sweep.py [--size WxH] [--frames N]

Exit code 1 means something is dead.

Things that will fool you here, and each one cost a run to find:

  * **The baseline Progress must sit inside the bitmap, not past it.** Past 6144
    bytes every pixel is on screen, so Background is correctly dead -- there is
    nothing unrevealed left for it to colour. 0.55 leaves about half the picture
    still to arrive.
  * **Message On needs a failure to be showing.** With no errors there is no
    message and the control is correctly dead. The CONTEXT below pins the
    (Error Rate, Progress) pair that puts a failure half a block back, and
    `pttest --message` asserts that pair still does -- so a change to the hash
    breaks loudly instead of quietly hollowing this out.
  * **Border Width needs Border On.** Border Off is not a colour, it is the
    absence of a border: the screen fills the composition and the width has
    nothing to apply to.
  * **Sync needs time to have passed AND a host that moves barPhase.** The
    harness renders 30 frames of a synthetic 60 fps clock and drives
    SetBeatInfo itself; with barPhase pinned at 0, which is the SDK's default,
    Beat and Bar correctly never move.
  * **The About block is not a control.** The text line and the link buttons are
    parameters only because FFGL has no window. They are skipped, not swept.
"""
import argparse, os, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("PTTEST", os.path.join(HERE, "build", "pttest"))
if not os.path.exists(BIN):
    alt = os.path.join(HERE, "build-dev", "pttest")
    if os.path.exists(alt):
        BIN = alt

SCRATCH = tempfile.mkdtemp(prefix="ptsweep")

# A baseline where every stage has something to do: a tape about half in, the
# border running, no errors.
BASE = {
    "Type": 0, "Baud": 0.5, "Ink": 0, "Paper": 7, "Bright": 1,
    "Progress": 0.55, "Sync": 0, "Error Rate": 0.0, "Message On": 1,
    "Border On": 1, "Border Width": 0.32, "Pilot Length": 0.16,
    "Mix": 1.0, "Background": 0,
}

# What a control needs around it before it means anything.
CONTEXT = {
    # Pinned by pttest --message: a block fails half a block of tape back, so
    # the report is on screen and Message On has something to switch off.
    "Message On": {"Error Rate": 0.30, "Progress": 0.54},
}

# Options are discrete; sweep them across their real element range rather than
# 0..1, which for an eight-entry list would only ever reach element 1.
DISCRETE = {
    "Type": (0, 3), "Ink": (0, 7), "Paper": (0, 7), "Bright": (0, 2),
    "Sync": (0, 3), "Background": (0, 2), "Message On": (0, 1), "Border On": (0, 1),
}

# Not controls: the About block exists only because FFGL has no window.
SKIP = {"About", "User guide", "Project page", "Source on GitHub", "Support the work"}

# How much of the picture a control has to move to count as alive. The default
# is half a per cent, which is the fleet's figure.
#
# Message On gets its own, and the number is arithmetic rather than a nudge: the
# report is 150x7 Spectrum pixels, which is 2.1% of the 256x192 screen; the
# screen is (1 - 2*0.08)^2 = 70.6% of the frame at the baseline border, so the
# whole box is 1.5% of it; and only the pixels where the glyph differs from what
# was underneath change at all, which is about a third of the box. A text
# overlay simply cannot move half a frame. 0.1% is a third of what it does move
# and a hundred times what a dead control would.
DEFAULT_THRESHOLD = 0.5
THRESHOLD = {"Message On": 0.1}


def render(path, overrides, size, frames):
    args = [BIN, "--out", path, "--size", size, "--frames", str(frames)]
    merged = dict(BASE)
    merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr)
        sys.exit(1)
    with open(path, "rb") as f:
        return f.read()


def pixels(png):
    i, idat, w, h = 8, b"", 0, 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t = png[i + 4:i + 8]
        d = png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa)
    changed = total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / (n / 4) * 100, total / (n / 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", default="960x540")
    # Thirty frames of the synthetic 60 fps clock: half a second, which is a
    # quarter of a bar at the harness's 120 bpm, so the Sync modes have
    # somewhere to have moved to.
    ap.add_argument("--frames", type=int, default=30)
    args = ap.parse_args()

    if not os.path.exists(BIN):
        print(f"pttest not found at {BIN}. Run: cmake --build build")
        return 1

    listing = subprocess.run([BIN, "--list"], capture_output=True, text=True).stdout
    params = [" ".join(l.split()[1:-1]) for l in listing.strip().splitlines()]
    params = [p for p in params if p not in SKIP]
    if not params:
        print("no parameters listed -- is the build current?")
        return 1

    print(f"{'parameter':<16} {'pixels changed':>15} {'mean delta':>11}   verdict")
    dead = []
    for p in params:
        lo, hi = DISCRETE.get(p, (0.0, 1.0))
        extra = CONTEXT.get(p, {})
        a = render(f"{SCRATCH}/a.png", {**extra, p: lo}, args.size, args.frames)
        b = render(f"{SCRATCH}/b.png", {**extra, p: hi}, args.size, args.frames)
        pct, mean = diff(a, b)
        ok = pct > THRESHOLD.get(p, DEFAULT_THRESHOLD)
        if not ok:
            dead.append(p)
        floor = THRESHOLD.get(p, DEFAULT_THRESHOLD)
        note = "" if floor == DEFAULT_THRESHOLD else f"  (floor {floor}%)"
        print(f"{p:<16} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}{note}")

    print()
    if dead:
        print("DEAD CONTROLS:", ", ".join(dead))
        return 1
    print(f"all {len(params)} parameters affect the output")
    return 0


if __name__ == "__main__":
    sys.exit(main())
