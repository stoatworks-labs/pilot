/**
 * Pilot — browser demo.
 *
 * A ZX Spectrum tape loader. The one idea, from `AGENTS.md`: **the address order
 * is the whole plugin.** The Spectrum's display file is 6144 bytes of bitmap and
 * then 768 of attributes, and the bitmap is addressed third, then line within a
 * character cell, then character row — so loading it in address order paints
 * the screen in an eight-line interleave, three times, in monochrome, and the
 * colour arrives in one block at the very end. The border is the loading signal
 * itself, painted by the beam as it scans, which is why it runs in bands.
 *
 * Pilot is built so that almost nothing is decided in the shader that could be
 * decided on the CPU: where the tape has got to, which blocks failed, whether
 * the error report is up and the border's phase all come from `Loader.cpp` as
 * uniforms. So this page is two halves, and they are not equally faithful.
 *
 * ---------------------------------------------------- what is the plugin's own
 *
 * The GPU half. `VERTEX`, `RASTER`, `ATTR` and `COMPOSE` below are `kVertex`,
 * `kRasterFragment`, `kAttrFragment` and `kComposeFragment` from
 * `source/shaders/`, copied across unedited. The compose shader carries the
 * third copy of the address order, the one that paints the screen.
 * `demo/tools/check_shaders.py` compares all four character for character, and
 * the glyphs and placement of the loading-error message against `Font.cpp` and
 * `Pilot.cpp`, and `tools/verify.sh` runs it.
 *
 * ---------------------------------------------------- what is a port
 *
 * The whole CPU chain, as galvo's was, because without it there is nothing to
 * show: `Loader.cpp` (the PCG hash, the per-block Bernoulli draw, `Evaluate`,
 * the border's bit position, the flash colour), `Machines.cpp`'s table,
 * `Controls.h`, `Spectrum.cpp`'s palette, `effectiveProgress()`, the message
 * bitmap `buildMessageTexture()` makes, and every uniform `ProcessOpenGL()` sets.
 * Where the C++ is `float` the port rounds through `Math.fround`, and the hash
 * is 32-bit exact through `Math.imul`, so a given tape fails in the same blocks
 * here as in the plugin; where the C++ is `double`, JavaScript already is.
 * Nothing checks the port but a reader. `pttest --agree`, `--order`,
 * `--border`, `--error`, `--message` and `--clock` check the C++ and have never
 * heard of this page.
 *
 * ---------------------------------------------------- what is not the plugin
 *
 * **The clock is the page's, in seconds.** The plugin votes on whether its host
 * sends seconds or milliseconds; here the page is the host and says seconds,
 * which is what `pttest` does through `ForceSecondsClock()`. It drives the
 * border's stripes and the Clip time / Beat / Bar sync modes. Pause stops them.
 *
 * **There is no host tempo.** Beat and Bar recover a bar count from the host's
 * tempo and bar phase. A browser has neither, so the page acts as a host
 * transport running at 120 bpm from the page's zero — the SDK's own default
 * tempo, and what `pttest` sends — and hands the plugin's recovery that bar
 * phase. Restart puts it back on the bar line.
 *
 * **The About block is absent**, as on every page in this suite.
 *
 * ---------------------------------------------------- decided, not asked
 *
 * **The glyphs are only the ones the message uses.** The plugin carries a whole
 * 5x7 font from graticule and draws one string from it; the page carries the
 * seventeen glyphs that string needs, and check_shaders.py holds each against
 * `Font.cpp` row for row.
 *
 * **There is a line under the canvas** reporting the tape: bytes in, the block,
 * the attempt, and which part of the display file is arriving. In Manual the
 * picture does not move by itself, and the line is what says that is the
 * plugin's design rather than a stalled page.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/shaders/*.cpp. Do not edit here.
//
// The backticks inside the comments are escaped, because a template literal has
// nowhere else to put them; check_shaders.py decodes that one escape and
// rejects any other backslash.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
`;

const RASTER = `#version 410 core
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputSize;
uniform vec2 TargetSize;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec2 ratio = InputSize / max( TargetSize, vec2( 1.0 ) );

	// One tap per source texel covered, capped so a 4K or 8K composition costs
	// a bounded amount. The cap only bites past an 8:1 reduction, and 8:1 onto
	// 256x192 is already a 2048x1536 source.
	ivec2 taps = ivec2( clamp( ceil( ratio ), vec2( 1.0 ), vec2( 8.0 ) ) );
	vec2 texel = MaxUV / max( InputSize, vec2( 1.0 ) );

	vec4 sum = vec4( 0.0 );
	for( int y = 0; y < taps.y; ++y )
	{
		for( int x = 0; x < taps.x; ++x )
		{
			vec2 f = ( vec2( x, y ) + 0.5 ) / vec2( taps ) - 0.5;
			sum += texture( InputTexture, uv + f * ratio * texel );
		}
	}

	vec4 color = sum / float( taps.x * taps.y );

	// Straight colour from here on. The attribute pass compares luminances, and
	// a premultiplied pixel that is dark only because it is transparent would
	// otherwise be read as a legitimately dark one and pull the cell's
	// threshold with it.
	if( color.a > 0.0 )
		color.rgb /= color.a;

	fragColor = color;
}
`;

const ATTR = `#version 410 core
uniform sampler2D RasterTexture;
uniform vec2 MaxUV;
uniform int BrightMode;//0 off, 1 auto, 2 on

in vec2 uv;

out vec4 fragColor;

const vec3 kLumaWeights = vec3( 0.299, 0.587, 0.114 );

// The eight hues are the corners of the RGB cube scaled by the level, so the
// nearest one is a per-channel comparison against half the level and no search
// is needed. Bit 2 is green, bit 1 red, bit 0 blue -- the Spectrum's own order.
int nearestColour( vec3 c, float level )
{
	float mid = level * 0.5;
	return ( c.g > mid ? 4 : 0 ) | ( c.r > mid ? 2 : 0 ) | ( c.b > mid ? 1 : 0 );
}

void main()
{
	// uv.y runs 0 at the BOTTOM of the texture, which is also the bottom of the
	// picture, and the raster texture below is stored the same way. This pass
	// therefore never flips anything: the flip to screen-top order happens once,
	// in the compose pass, where the address arithmetic needs it.
	ivec2 cell = clamp( ivec2( floor( uv * vec2( 32.0, 24.0 ) ) ), ivec2( 0 ), ivec2( 31, 23 ) );
	ivec2 base = cell * 8;

	float lo = 2.0;
	float hi = -1.0;
	for( int y = 0; y < 8; ++y )
	{
		for( int x = 0; x < 8; ++x )
		{
			float l = dot( texelFetch( RasterTexture, base + ivec2( x, y ), 0 ).rgb, kLumaWeights );
			lo = min( lo, l );
			hi = max( hi, l );
		}
	}

	float midpoint = ( lo + hi ) * 0.5;
	float contrast = clamp( ( hi - lo ) * 8.0, 0.0, 1.0 );
	float threshold = mix( 0.5, midpoint, contrast );

	vec3 darkSum = vec3( 0.0 );
	vec3 lightSum = vec3( 0.0 );
	float darkN = 0.0;
	float lightN = 0.0;
	float peak = 0.0;
	for( int y = 0; y < 8; ++y )
	{
		for( int x = 0; x < 8; ++x )
		{
			vec3 c = texelFetch( RasterTexture, base + ivec2( x, y ), 0 ).rgb;
			peak = max( peak, max( c.r, max( c.g, c.b ) ) );
			if( dot( c, kLumaWeights ) < threshold )
			{
				darkSum += c;
				darkN += 1.0;
			}
			else
			{
				lightSum += c;
				lightN += 1.0;
			}
		}
	}

	// BRIGHT is one bit for the whole cell, so it is decided by the brightest
	// thing in the cell rather than by either colour on its own. The crossover
	// sits half way between the two levels the hardware had, 0xD7 and 0xFF.
	bool bright = BrightMode == 2;
	if( BrightMode == 1 )
		bright = peak > ( ( 215.0 / 255.0 ) + 1.0 ) * 0.5;
	float level = bright ? 1.0 : ( 215.0 / 255.0 );

	vec3 inkColour   = darkN  > 0.0 ? darkSum  / darkN  : vec3( 0.0 );
	vec3 paperColour = lightN > 0.0 ? lightSum / lightN : vec3( 1.0 );

	fragColor = vec4( float( nearestColour( inkColour, level ) ),
	                  float( nearestColour( paperColour, level ) ),
	                  bright ? 1.0 : 0.0,
	                  threshold );
}
`;

const COMPOSE = `#version 410 core
uniform sampler2D RasterTexture;
uniform sampler2D AttrTexture;
uniform sampler2D InputTexture;
uniform sampler2D MessageTexture;

uniform vec2 MaxUV;
uniform vec2 InputMaxUV;
uniform vec2 Inset;

uniform int BytesRevealed;
uniform vec3 DefaultInk;
uniform vec3 DefaultPaper;

uniform int BorderStyle;     //0 scanned stripes, 1 whole-border flash
uniform float BorderHalfPhase;//half-cycles at the top of the frame, reduced to [0, 2)
uniform float BorderHalfSpan; //half-cycles covered by one frame's scan
uniform vec3 BorderA;
uniform vec3 BorderB;

uniform int MessageOn;
uniform vec2 MessageOrigin;//Spectrum pixels, y from the top
uniform vec2 MessageSize;

uniform int Background;//0 paper, 1 black, 2 the clip
uniform float Mix;

in vec2 uv;

out vec4 fragColor;

const vec3 kLumaWeights = vec3( 0.299, 0.587, 0.114 );

/// Colour \`c\` (0..7) at the basic or bright level. Bit 2 green, bit 1 red,
/// bit 0 blue, which is the order the Spectrum's hardware wires the guns in.
vec3 zxColour( int c, bool bright )
{
	float level = bright ? 1.0 : ( 215.0 / 255.0 );
	return level * vec3( float( ( c >> 1 ) & 1 ), float( ( c >> 2 ) & 1 ), float( c & 1 ) );
}

/// The display file's address for a pixel. The bit layout, exactly as
/// pilot::zx::ScreenIndex states it.
int screenIndex( int px, int py )
{
	int xbyte   = px >> 3;
	int third   = py >> 6;
	int charrow = ( py >> 3 ) & 7;
	int line    = py & 7;
	return third * 2048 + line * 256 + charrow * 32 + xbyte;
}

vec3 borderColour( float rowFromTop )
{
	if( BorderStyle == 1 )
		return BorderA;//one colour for the whole border, changed once per byte

	// Both terms are non-negative -- BorderHalfPhase is reduced to [0, 2) on the
	// CPU and rowFromTop*BorderHalfSpan cannot be negative -- so the int
	// conversion and the & are well defined. GLSL leaves % and / undefined on
	// negative operands, which is why this is a mask and not a modulo.
	float halfCycle = floor( BorderHalfPhase + rowFromTop * BorderHalfSpan );
	return ( int( halfCycle ) & 1 ) != 0 ? BorderA : BorderB;
}

void main()
{
	vec2 p = uv;
	vec4 clip = texture( InputTexture, p * InputMaxUV );

	vec2 span = max( vec2( 1.0 ) - 2.0 * Inset, vec2( 1e-5 ) );
	vec2 inner = ( p - Inset ) / span;

	vec3 col;

	if( inner.x < 0.0 || inner.x >= 1.0 || inner.y < 0.0 || inner.y >= 1.0 )
	{
		col = borderColour( 1.0 - p.y );
	}
	else
	{
		int px = clamp( int( inner.x * 256.0 ), 0, 255 );
		int py = clamp( int( ( 1.0 - inner.y ) * 192.0 ), 0, 191 );
		int cx = px >> 3;
		int cy = py >> 3;

		vec4 attr = texelFetch( AttrTexture, ivec2( cx, 23 - cy ), 0 );
		vec3 src  = texelFetch( RasterTexture, ivec2( px, 191 - py ), 0 ).rgb;

		// Which of the cell's two colours this pixel takes. Decided from the
		// picture, NOT from whether the attribute has arrived: on a real tape
		// the bit pattern was settled when the screen was converted, and the
		// attribute byte only says what the two colours are.
		bool isInk = dot( src, kLumaWeights ) < attr.a;

		bool attrHere = ( 6144 + cy * 32 + cx ) < BytesRevealed;
		vec3 ink   = attrHere ? zxColour( int( attr.r + 0.5 ), attr.b > 0.5 ) : DefaultInk;
		vec3 paper = attrHere ? zxColour( int( attr.g + 0.5 ), attr.b > 0.5 ) : DefaultPaper;

		if( screenIndex( px, py ) < BytesRevealed )
		{
			col = isInk ? ink : paper;
		}
		else if( Background == 1 )
		{
			col = vec3( 0.0 );
		}
		else if( Background == 2 )
		{
			col = clip.rgb;
		}
		else
		{
			col = DefaultPaper;
		}

		if( MessageOn == 1 )
		{
			vec2 m = vec2( float( px ), float( py ) ) - MessageOrigin;
			if( m.x >= 0.0 && m.x < MessageSize.x && m.y >= 0.0 && m.y < MessageSize.y )
			{
				// The message texture is uploaded top row first, so no flip.
				float lit = texelFetch( MessageTexture, ivec2( int( m.x ), int( m.y ) ), 0 ).r;
				col = lit > 0.5 ? DefaultInk : DefaultPaper;
			}
		}
	}

	fragColor = vec4( mix( clip.rgb, col, Mix ), mix( clip.a, 1.0, Mix ) );
}
`;

//---------------------------------------------------------------------------
// The glyphs the loading-error message uses, copied from Font.cpp (which took
// them from graticule). check_shaders.py holds each row against Font.cpp.
//---------------------------------------------------------------------------
const GLYPHS = {
  32: ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // ' '
  44: ['.....', '.....', '.....', '.....', '.##..', '..#..', '.#...'], // ','
  48: ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'], // '0'
  49: ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'], // '1'
  58: ['.....', '.##..', '.##..', '.....', '.##..', '.##..', '.....'], // ':'
  82: ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'], // 'R'
  84: ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 'T'
  97: ['.....', '.....', '.###.', '....#', '.####', '#...#', '.####'], // 'a'
  100: ['....#', '....#', '.##.#', '#..##', '#...#', '#...#', '.####'], // 'd'
  101: ['.....', '.....', '.###.', '#...#', '#####', '#....', '.###.'], // 'e'
  103: ['.....', '.....', '.####', '#...#', '.####', '....#', '.###.'], // 'g'
  105: ['..#..', '.....', '.##..', '..#..', '..#..', '..#..', '.###.'], // 'i'
  108: ['.##..', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 'l'
  110: ['.....', '.....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 'n'
  111: ['.....', '.....', '.###.', '#...#', '#...#', '#...#', '.###.'], // 'o'
  112: ['.....', '.....', '####.', '#...#', '####.', '#....', '#....'], // 'p'
  114: ['.....', '.....', '#.##.', '##..#', '#....', '#....', '#....'], // 'r'
};

//===========================================================================
// The port. Spectrum.cpp's constants and palette.
//===========================================================================

const f32 = Math.fround;
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
/// std::lround: half away from zero.
const lround = (x) => Math.sign(x) * Math.round(Math.abs(x));

const kScreenW = 256;
const kScreenH = 192;
const kCellsX = kScreenW / 8;
const kCellsY = kScreenH / 8;
const kDisplayBytes = 6144;
const kAttributeBytes = 768;
const kTotalBytes = kDisplayBytes + kAttributeBytes; // 6912
const kBlockBytes = 256;
const kBlocks = kTotalBytes / kBlockBytes; // 27

const kBasicLevel = 215;
const kBrightLevel = 255;

/// Colour `index` (0..7): bit 2 green, bit 1 red, bit 0 blue.
function Colour(index, bright) {
  const level = bright ? kBrightLevel : kBasicLevel;
  const i = index & 7;
  return { r: ((i >> 1) & 1) * level, g: ((i >> 2) & 1) * level, b: (i & 1) * level };
}

const ColourNames = ['Black', 'Blue', 'Red', 'Magenta', 'Green', 'Cyan', 'Yellow', 'White'];

//===========================================================================
// Machines.cpp — four loaders as a table. Only ZX 48 models a documented
// routine; the other three are honestly distinct looks.
//===========================================================================

const kBorderScanned = 0;
const kBorderFlash = 1;

const kMachines = [
  //  name          baud    fps   style           pilot   data   bright
  { name: 'ZX 48', nominalBaud: 1500.0, frameHz: 50.0, border: kBorderScanned, pilotA: 2, pilotB: 5, dataA: 6, dataB: 1, bright: false },
  { name: 'ZX 128', nominalBaud: 2250.0, frameHz: 50.0, border: kBorderScanned, pilotA: 3, pilotB: 4, dataA: 7, dataB: 0, bright: false },
  { name: 'C64 turbo', nominalBaud: 2400.0, frameHz: 50.0, border: kBorderFlash, pilotA: 0, pilotB: 0, dataA: 0, dataB: 0, bright: false },
  { name: 'Amstrad', nominalBaud: 2000.0, frameHz: 50.0, border: kBorderScanned, pilotA: 5, pilotB: 1, dataA: 2, dataB: 6, bright: true },
];

const machine = (index) => kMachines[index < 0 || index >= kMachines.length ? 0 : index];

//===========================================================================
// Controls.h. Float in the plugin, so float here.
//===========================================================================

const BaudMultiplier = (v) => f32(Math.pow(2, f32((clamp(f32(v), 0, 1) - 0.5) * 4.0)));
const Baud = (nominal, v) => f32(nominal * BaudMultiplier(v));
const ErrorRate = (v) => f32(clamp(f32(v), 0, 1) * 0.5);
const BorderInset = (v) => f32(clamp(f32(v), 0, 1) * 0.25);
const PilotLength = (v) => f32(clamp(f32(v), 0, 1) * 0.5);

/// Double in the plugin.
function TapeSeconds(baud, pilotFraction) {
  const dataSeconds = baud > 1.0 ? (6912.0 * 8.0) / baud : 36.0;
  const f = clamp(pilotFraction, 0.0, 0.95);
  return dataSeconds / Math.max(1.0 - f, 0.05);
}

//===========================================================================
// Loader.cpp — the tape, as a pure function.
//===========================================================================

/// PCG output-mixed integer hash, exact in 32 bits.
function HashInt(x) {
  x = (Math.imul(x >>> 0, 747796405) + 2891336453) >>> 0;
  const w = Math.imul(((x >>> ((x >>> 28) + 4)) ^ x) >>> 0, 277803737) >>> 0;
  return ((w >>> 22) ^ w) >>> 0;
}

function Hash3(a, b, c) {
  const inner = HashInt((Math.imul(c >>> 0, 40503) + 1) >>> 0);
  return HashInt(((a >>> 0) ^ HashInt((Math.imul(b >>> 0, 2654435761 | 0) ^ inner) >>> 0)) >>> 0);
}

/// static_cast< float >( x ) * ( 1.0f / 4294967296.0f ): the cast rounds to 24
/// bits, and the scale is a power of two, so it is exact after the fround.
const Hash01 = (x) => f32(x) / 4294967296.0;

function BlockFails(seed, attempt, block, rate) {
  if (rate <= 0.0) return false;
  if (rate >= 1.0) return true;
  return Hash01(Hash3(seed, attempt, block)) < rate;
}

const kMessageHold = 1.0;

function Evaluate({ progress, pilotLength, errorRate, seed = 1 }) {
  const out = { pilot: true, tapeProgress: 0, bytesRevealed: 0, attempt: 0, blocksLoaded: 0, sinceFailure: -1, message: false };

  const p = clamp(f32(progress), 0.0, 1.0);
  const pilot = clamp(f32(pilotLength), 0.0, 0.95);

  out.pilot = p < pilot;
  out.tapeProgress = pilot >= 1.0 ? 0.0 : clamp(f32(f32(p - pilot) / f32(1.0 - pilot)), 0.0, 1.0);

  // Double from here, as in the C++.
  const q = out.tapeProgress * kBlocks;
  const whole = Math.min(Math.floor(q), kBlocks);
  const frac = q - Math.floor(q);

  let attempt = 0;
  let inAttempt = 0;
  let lastFail = -1.0;
  let heldAtFail = 0;

  for (let k = 0; k < whole; k += 1) {
    if (BlockFails(seed, attempt, inAttempt, errorRate)) {
      heldAtFail = inAttempt;
      attempt += 1;
      inAttempt = 0;
      lastFail = k + 1.0;
    } else {
      inAttempt += 1;
    }
  }

  out.attempt = attempt;
  out.blocksLoaded = inAttempt;

  const loaded = inAttempt + (whole < kBlocks ? frac : 0.0);
  out.bytesRevealed = clamp(Math.floor(loaded * kBlockBytes), 0, kTotalBytes);

  if (lastFail >= 0.0) {
    out.sinceFailure = f32(q - lastFail);
    out.message = out.sinceFailure < kMessageHold;
    // A real Spectrum prints the report over whatever had arrived: the byte
    // count HOLDS while the message is up.
    if (out.message) out.bytesRevealed = clamp(heldAtFail * kBlockBytes, 0, kTotalBytes);
  }

  if (out.pilot) {
    out.bytesRevealed = 0;
    out.message = false;
    out.sinceFailure = -1.0;
    out.attempt = 0;
    out.blocksLoaded = 0;
  }

  return out;
}

function BorderAt(seconds, baud, frameHz) {
  return { bitPosTop: seconds * baud, bitsPerFrame: frameHz > 0.0 ? baud / frameHz : 0.0 };
}

const HalfCycleContinuous = (b, f) => (b.bitPosTop + f * b.bitsPerFrame) * 2.0;
const ByteIndex = (b) => Math.floor(b.bitPosTop / 8.0);

/// Never black. static_cast< uint32_t > of the byte index is modulo 2^32,
/// which is what `>>> 0` is.
const FlashColour = (byteIndex) => 1 + (HashInt(byteIndex >>> 0) % 7);

//===========================================================================
// Pilot.cpp — the message, the clock's use, effectiveProgress().
//===========================================================================

const kErrorMessage = 'R Tape loading error, 0:1';
const kMessageX = 4;
const kMessageY = 184;
const kWidth = 5;
const kHeight = 7;
const kAdvance = kWidth + 1;
const messageWidth = () => kErrorMessage.length * kAdvance;

const kSyncManual = 0;
const kSyncClip = 1;
const kSyncBeat = 2;
const kBeatsPerBar = 4.0;

/// The tempo the page's transport runs at: the SDK's default, and pttest's.
const kPageBpm = 120.0;

/// The bar phase a host transport at kPageBpm, started at the page's zero,
/// would send. This is the page standing in for SetBeatInfo, not the plugin.
function pageBarPhase(seconds) {
  const bars = seconds / ((60.0 * kBeatsPerBar) / kPageBpm);
  return f32(bars - Math.floor(bars));
}

function effectiveProgress(params, seconds) {
  const manual = clamp(f32(params.get('progress')), 0.0, 1.0);
  const mode = lround(params.get('sync'));
  if (mode === kSyncManual) return manual;

  let turns = 0.0;
  if (mode === kSyncClip) {
    const spec = machine(lround(params.get('type')));
    const baud = Baud(spec.nominalBaud, params.get('baud'));
    const period = TapeSeconds(baud, PilotLength(params.get('pilotLength')));
    turns = seconds / Math.max(period, 0.001);
  } else {
    // The fleet's bar recovery: the clock estimates the bar count, barPhase is
    // exact inside the bar, and round( estimate - within ) reconciles them.
    const tempo = kPageBpm > 1.0 ? kPageBpm : 120.0;
    const barSeconds = (60.0 * kBeatsPerBar) / tempo;
    const estimate = seconds / barSeconds;
    const within = clamp(pageBarPhase(seconds), 0.0, 1.0);
    const bars = within + Math.round(estimate - within);
    turns = mode === kSyncBeat ? bars * kBeatsPerBar : bars;
  }

  // Wrapped with floor, not a cast.
  const raw = manual + turns;
  return f32(raw - Math.floor(raw));
}

/// What the line under the canvas reports. Written by the renderer.
const telemetry = { tape: null, baud: 0, pairs: 0, machine: '' };

//===========================================================================
// The passes, in ProcessOpenGL's order.
//===========================================================================

function createRenderer(gl, quad) {
  const rasterShader = new Program(gl, VERTEX, RASTER, 'raster');
  const attrShader = new Program(gl, VERTEX, ATTR, 'attr');
  const composeShader = new Program(gl, VERTEX, COMPOSE, 'compose');

  const rasterBuffer = new PassBuffer(gl, { filter: 'nearest' });
  const attrBuffer = new PassBuffer(gl, { filter: 'nearest' });

  // buildMessageTexture(): one byte per pixel, row 0 the TOP row of the glyphs.
  const mw = messageWidth();
  const bitmap = new Uint8Array(mw * kHeight);
  for (let c = 0; c < kErrorMessage.length; c += 1) {
    const glyph = GLYPHS[kErrorMessage.charCodeAt(c)];
    for (let y = 0; y < kHeight; y += 1) {
      for (let x = 0; x < kWidth; x += 1) {
        if (glyph[y][x] === '#') bitmap[y * mw + c * kAdvance + x] = 255;
      }
    }
  }
  const messageTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, messageTexture);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, mw, kHeight, 0, gl.RED, gl.UNSIGNED_BYTE, bitmap);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  return {
    render({ input, params, width, height, time }) {
      const p = (id) => params.get(id);

      rasterBuffer.ensure(kScreenW, kScreenH, gl.RGBA8);
      attrBuffer.ensure(kCellsX, kCellsY, gl.RGBA16F);

      const spec = machine(lround(p('type')));
      const baud = Baud(spec.nominalBaud, p('baud'));
      const seconds = time;

      const tape = Evaluate({
        progress: effectiveProgress(params, seconds),
        pilotLength: PilotLength(p('pilotLength')),
        errorRate: ErrorRate(p('errorRate')),
      });

      gl.disable(gl.BLEND);

      // 1. The clip, down onto 256x192. The kit's clip fills its texture.
      rasterBuffer.bind();
      rasterShader.use();
      bindTexture(gl, 0, input.texture);
      rasterShader.setSampler('InputTexture', 0);
      rasterShader.set('MaxUV', 1.0, 1.0);
      rasterShader.set('InputSize', input.width, input.height);
      rasterShader.set('TargetSize', kScreenW, kScreenH);
      quad.draw();

      // 2. One texel per character cell: its two colours and its threshold.
      attrBuffer.bind();
      attrShader.use();
      bindTexture(gl, 0, rasterBuffer.texture);
      attrShader.setSampler('RasterTexture', 0);
      attrShader.set('MaxUV', 1.0, 1.0);
      attrShader.setInt('BrightMode', lround(p('bright')));
      quad.draw();

      // 3. The output, at the host's viewport.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      composeShader.use();
      bindTexture(gl, 0, rasterBuffer.texture);
      bindTexture(gl, 1, attrBuffer.texture);
      bindTexture(gl, 2, input.texture);
      bindTexture(gl, 3, messageTexture);
      composeShader.setSampler('RasterTexture', 0);
      composeShader.setSampler('AttrTexture', 1);
      composeShader.setSampler('InputTexture', 2);
      composeShader.setSampler('MessageTexture', 3);
      composeShader.set('MaxUV', 1.0, 1.0);
      composeShader.set('InputMaxUV', 1.0, 1.0);

      // Border Off is not a colour, it is the absence of a border.
      const inset = p('borderOn') > 0.5 ? BorderInset(p('borderWidth')) : 0.0;
      composeShader.set('Inset', inset, inset);

      composeShader.setInt('BytesRevealed', tape.bytesRevealed);

      const defaultBright = lround(p('bright')) === 2;
      const ink = Colour(lround(p('ink')), defaultBright);
      const paper = Colour(lround(p('paper')), defaultBright);
      composeShader.set('DefaultInk', ink.r / 255.0, ink.g / 255.0, ink.b / 255.0);
      composeShader.set('DefaultPaper', paper.r / 255.0, paper.g / 255.0, paper.b / 255.0);

      // The border's phase, reduced to [0, 2) in double before it reaches the
      // shader, so the shader never sees a large number.
      const border = BorderAt(seconds, baud, spec.frameHz);
      const halfTop = HalfCycleContinuous(border, 0.0);
      let reduced = halfTop % 2.0;
      if (reduced < 0.0) reduced += 2.0;

      composeShader.setInt('BorderStyle', spec.border === kBorderFlash ? 1 : 0);
      composeShader.set('BorderHalfPhase', reduced);
      composeShader.set('BorderHalfSpan', border.bitsPerFrame * 2.0);

      let colourA = tape.pilot ? spec.pilotA : spec.dataA;
      let colourB = tape.pilot ? spec.pilotB : spec.dataB;
      if (spec.border === kBorderFlash) {
        colourA = FlashColour(ByteIndex(border));
        colourB = colourA;
      }
      const a = Colour(colourA, spec.bright);
      const b = Colour(colourB, spec.bright);
      composeShader.set('BorderA', a.r / 255.0, a.g / 255.0, a.b / 255.0);
      composeShader.set('BorderB', b.r / 255.0, b.g / 255.0, b.b / 255.0);

      const showMessage = p('message') > 0.5 && tape.message;
      composeShader.setInt('MessageOn', showMessage ? 1 : 0);
      composeShader.set('MessageOrigin', kMessageX, kMessageY);
      composeShader.set('MessageSize', mw, kHeight);

      composeShader.setInt('Background', lround(p('background')));
      composeShader.set('Mix', clamp(p('mix'), 0.0, 1.0));

      quad.draw();

      telemetry.tape = tape;
      telemetry.baud = baud;
      telemetry.pairs = border.bitsPerFrame;
      telemetry.machine = spec.name;
      telemetry.flash = spec.border === kBorderFlash;
    },
  };
}

//===========================================================================
// The controls, read out of Pilot's constructor. Same names, same groups, same
// order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({
  id, name, type: 'standard', default: def, group,
  ...(typeof extra === 'string' ? { hint: extra } : extra),
});
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const demo = mountDemo({
  name: 'Pilot',
  pluginId: 'PT01',
  tagline:
    'A tape loader. The clip arrives the way a ZX Spectrum loaded a screen: in display-file order, which is not top to bottom but three thirds, each filling in an eight-line interleave — and in monochrome, because the colour attributes come last, in one block. The border is the loading signal itself, painted by the beam as it scans, so it runs in bands. A transition, in practice: drive Progress by hand, by clip time, or off the beat. The shaders are the plugin’s own; the tape model is a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/pilot',
  page: 'https://stoatworks-labs.com/software/pilot/',
  video: 'https://www.youtube.com/watch?v=c9zzg9t-Epc',
  blurb:
    'It is Pilot’s own GLSL, ported from the repository to WebGL2, with its tape model — the block failures, the border signal, the machine table — ported to JavaScript, running on generated clips in this page.',

  // Mix below 1 carries the clip's alpha through.
  showBackdrop: true,

  // The attribute buffer is RGBA16F: the per-cell threshold decides a
  // per-pixel comparison, and eight bits would move every low-contrast cell.
  needFloat: true,

  params: [
    opt('type', 'Type', kMachines.map((m) => m.name), 0, 'Machine',
      'Which loader. Only ZX 48 models a documented routine — the 48K ROM’s 1500 baud and its red/cyan then yellow/blue border. ZX 128 is a turbo loader on Spectrum hardware, C64 turbo flashes the whole border once per byte, and Amstrad is the CPC’s 2000-baud rate. The other three are honestly distinct looks, not measurements.'),
    std('baud', 'Baud', 0.5, 'Machine', {
      display: (v) => `×${BaudMultiplier(v).toFixed(2)}`,
      hint: 'A trim around the machine’s own rate: ×0.25 to ×4. In Manual it sets only the border’s stripe rate; at Sync = Clip time the tape runs at it.',
    }),
    opt('ink', 'Ink', ColourNames, 0, 'Machine', 'The default attribute’s ink, before the attribute bytes arrive.'),
    opt('paper', 'Paper', ColourNames, 7, 'Machine', 'The default attribute’s paper. A Spectrum powers up black on white.'),
    opt('bright', 'Bright', ['Off', 'Auto', 'On'], 1, 'Machine',
      'BRIGHT is one bit of hardware. On sets it on the default attribute; Auto lets each cell decide from the brightest thing in it.'),

    std('progress', 'Progress', 0.45, 'Load', {
      display: (v) => `${(v * 100).toFixed(0)}%`,
      hint: 'Where the load has got to. The first Pilot Length of it is pilot tone, with nothing on the screen; the rest maps onto 6912 bytes. In the sync modes it is the offset the clock is added to.',
    }),
    opt('sync', 'Sync', ['Manual', 'Clip time', 'Beat', 'Bar'], 0, 'Load',
      'Manual: nothing moves by itself. Clip time: the tape runs at the baud rate — a ZX 48 load is 37 seconds. Beat and Bar: one whole load per beat or per bar. This page has no host tempo and runs its own transport at 120 bpm.'),
    std('errorRate', 'Error Rate', 0.0, 'Load', {
      display: (v) => `${(ErrorRate(v) * 100).toFixed(1)}% per block`,
      hint: 'The chance each 256-byte block fails. A failure restarts the load and the screen holds what had arrived while the report is up. Seeded, so the same tape fails in the same places every time.',
    }),
    bool('message', 'Message On', 1, 'Load', 'Print “R Tape loading error, 0:1” when a block fails.'),

    bool('borderOn', 'Border On', 1, 'Border', 'Off is not a colour: it is no border, and the screen fills the composition.'),
    std('borderWidth', 'Border Width', 0.32, 'Border', {
      display: (v) => `${(BorderInset(v) * 100).toFixed(1)}% each edge`,
      hint: 'How much of the composition is border, off each edge.',
    }),
    std('pilotLength', 'Pilot Length', 0.16, 'Border', {
      display: (v) => `${(PilotLength(v) * 100).toFixed(0)}% of Progress`,
      hint: 'How much of the Progress range is spent on the pilot tone before the first byte lands.',
    }),

    std('mix', 'Mix', 1.0, 'Output'),
    opt('background', 'Background', ['Paper', 'Black', 'Clip'], 0, 'Output',
      'What a byte that has not arrived shows.'),
  ],

  sources: ['scene', 'bars', 'grid', 'spot', 'ramp', 'alpha'],

  // The plugin ships no factory presets, so these are the page's own —
  // expressed entirely in the plugin's parameters and reachable with the sliders.
  presets: {
    'Watch it load (Clip time, ×4)': { sync: 1, baud: 1.0, progress: 0 },
    'A real ZX 48 load (37 s)': { sync: 1, progress: 0 },
    'One load per bar (120 bpm)': { sync: 3, progress: 0 },
    'Just the bitmap (monochrome)': { progress: 0.85 },
    'Colour arriving': { progress: 0.96 },
    'In the pilot tone': { progress: 0.05 },
    'A bad tape': { sync: 1, baud: 1.0, progress: 0, errorRate: 0.3 },
    'C64 turbo flash': { type: 2, sync: 1, baud: 1.0, progress: 0 },
    'Over the clip': { background: 2, progress: 0.3 },
  },

  differences: [
    'The three passes are the plugin’s own GLSL — the raster, the attribute file and the compose pass with the address order in it — and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the four shader strings drifts, or if the message’s glyphs or placement drift from Font.cpp and Pilot.cpp.',
    'The CPU half is a PORT, not the plugin’s code: Loader.cpp (the hash, the block failures, the border’s bit position, the flash colour), the Machines.cpp table, Controls.h, the palette, effectiveProgress() and every uniform ProcessOpenGL() sets. It rounds through float where the plugin is float, so the same tape fails in the same blocks. Nothing checks that port but a reader; pttest checks the C++.',
    'The clock is the page’s own, in seconds. It drives the border stripes and the sync modes; Pause stops both. Beat and Bar need the host’s tempo and bar phase, which a browser does not have, so the page runs a 120 bpm transport from its own zero — the SDK’s default tempo, and what the plugin’s harness sends.',
    'The border is the plugin’s arithmetic, but a browser presents at the display’s refresh, not at 50 Hz: the stripe count down the screen is the machine’s (baud ÷ 50 pairs), and how they seem to crawl depends on your display.',
    'Pilot has no audio input, so there is no audio caveat beyond that one — the loading signal here is drawn, never heard.',
    'The plugin’s proofs — two independent derivations of the address order agreeing bitwise, 49,152 rendered pixels against the table at two rasters, the error rate against the geometric mean, the border period against the byte period six days into a host clock — are pttest in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The tape line. Reports the model's state; measures nothing. Skipped in embed
// mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    const n = (x) => x.toLocaleString('en-GB');
    setInterval(() => {
      const { tape, baud, pairs, machine: name, flash } = telemetry;
      if (!tape) return;
      let where;
      if (tape.pilot) where = 'in the pilot tone: nothing on the screen yet';
      else if (tape.message) where = `block failed — the report is up and the screen holds at ${n(tape.bytesRevealed)} bytes`;
      else if (tape.bytesRevealed >= kTotalBytes) where = 'loaded';
      else if (tape.bytesRevealed >= kDisplayBytes) where = `attributes arriving: ${n(tape.bytesRevealed - kDisplayBytes)} of 768`;
      else where = `display file, third ${Math.floor(tape.bytesRevealed / 2048) + 1} of 3, pixel line ${Math.floor((tape.bytesRevealed % 2048) / 256)} of every character row`;
      const border = flash ? 'the whole border changes once a byte' : `${pairs.toFixed(0)} stripe pairs a frame`;
      line.textContent =
        `${name} at ${n(Math.round(baud))} baud, ${border}. `
        + `${n(tape.bytesRevealed)} of 6,912 bytes, attempt ${tape.attempt + 1}: ${where}.`;
    }, 250);
  }
}
