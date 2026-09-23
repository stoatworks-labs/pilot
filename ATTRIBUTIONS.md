# Attributions

Pilot is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### ZX Spectrum palette and attribute model — Stoatworks nesolume

<https://github.com/stoatworks-labs/nesolume>  
Licence: MIT  
Copyright: Stoatworks Labs

Same fleet, copied rather than shared: source/Spectrum.cpp takes the two hardware levels (0xD7 basic, 0xFF bright) and the GRB bit order from the ZX Spectrum rows of nesolume's source/Consoles.cpp, and the 8×8 attribute model — one ink, one paper and a BRIGHT bit per character cell — is the one nesolume's quantise stage implements. How the two colours are chosen is not from nesolume: nesolume models attribute clash, while pilot splits each cell at a threshold and takes the mean colour either side, modelling a screen converted before it went on tape.

### Offline test card — Stoatworks nesolume

<https://github.com/stoatworks-labs/nesolume>  
Licence: MIT  
Copyright: Stoatworks Labs

The bands of tools/pttest's default picture — hue × brightness, overlapping discs on grey, a luminance ramp and a fine checkerboard — are the ones nesolume's harness uses, for the same reason: each makes a different kind of wrong answer visible.

### 5×7 bitmap font — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Font.{h,cpp} is graticule's font table, unchanged except for the namespace. It draws one string here: the Spectrum's R Tape loading error, 0:1.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The ZX Spectrum tape loader

The display file's address layout, the 6912-byte screen, the attribute block arriving last and the 48K ROM loader's border colours are published facts about a machine from 1982, implemented from descriptions of how the hardware behaved. No ROM, no BIOS and no emulator code is present. The other three machines are loaders in the same shape with their own rates and colours; only the ZX 48 claims to model a documented routine.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
