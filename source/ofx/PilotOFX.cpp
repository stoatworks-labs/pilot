/// The OpenFX build of Pilot, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts. One plugin, two shapes: a **filter** -- the clip loads
/// itself, as in Resolume -- and a **transition**, which is what the effect is
/// in practice and what an OpenFX host can offer that an FFGL effect cannot.
///
/// ------------------------------------------------------- what is shared
///
/// Everything that is not per-pixel is the FFGL build's own code, linked
/// straight in: the address order (`Spectrum.cpp`), the tape and its failures
/// (`Loader.cpp`), the machines, the controls, the font, and every uniform the
/// three passes are given (`Frame.cpp` -- the same function `Pilot.cpp` calls).
/// The defaults come from the same struct. What *is* mirrored is the GLSL, in
/// `Render.cpp`, marked `//= mirrored:` in both places and compared pixel by
/// pixel against the GPU by `pttest --cpu`.
///
/// This file is marshalling: the host's pixel formats in, the passes run, the
/// host's pixel format back out. Its only per-pixel arithmetic is the
/// conversion.
///
/// ------------------------------------------------------------ the clock
///
/// OpenFX renders frames alone, out of order and on several threads, and this
/// effect was already built for that: `Loader.h` keeps no state between
/// frames, so a frame is a function of the controls and a time. OFX hands the
/// time in FRAMES, and the border and Clip time sync want seconds, so it is
/// divided by the output's frame rate -- which makes a frame render the same
/// however the host reaches it.
///
/// --------------------------------------------------------- what is missing
///
/// **Sync = Beat and Bar.** Resolume tells an FFGL plugin the tempo and where
/// it is in the bar; an OpenFX host tells a plugin neither. Manual (keyframe
/// Progress) and Clip time (a pure function of time) carry over, and the two
/// beat modes are left out of the menu rather than listed and dead -- they were
/// the last two entries, so nothing else renumbers. The plugin description
/// says so.
///
/// ------------------------------------------------------------ the transition
///
/// SourceTo is the picture being loaded and the host's Transition parameter is
/// Progress. SourceFrom is what was on screen before the tape started, and it
/// takes the one role in the compose pass the FFGL build gives the clip a
/// second time: what Background = Clip shows through an address that has not
/// arrived, and what Mix fades against. In the filter that is the Source
/// again, so the two shapes are one function with one substitution
/// (`render::Transition`), not two renderers. Progress and Sync are not
/// declared in the transition -- the host owns the position -- and Background
/// defaults to Clip there, since under Paper the outgoing shot would never
/// appear at all.
///
/// The transition's last frame is the loaded Spectrum screen, not SourceTo:
/// the edit then cuts to the real picture. That is the effect, not a defect --
/// at Progress 1 the FFGL build shows the same screen -- and it is said in the
/// description so nobody mistakes it for a dropped frame.
///
/// --------------------------------------------------------------- and tiles
///
/// The raster is a box filter of the whole picture and the attribute pass
/// needs whole cells of it, so there is no tile smaller than the frame.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Frame.h"
#include "../Machines.h"
#include "../Render.h"
#include "../Spectrum.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.pilot";
constexpr const char* kPluginName       = "Pilot";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A tape loader. The clip arrives the way a ZX Spectrum loaded it: in "
	"screen-memory order, at the baud rate, with the border painted by the "
	"loading signal itself.\n\n"
	"The Spectrum's display file is not linear, so the picture does not wipe "
	"down the screen - it arrives in three thirds, each filling in an "
	"eight-line interleave. Colour comes last, in one block of attributes, so "
	"the image lands in monochrome and colours in at the very end.\n\n"
	"As an effect, keyframe Progress or set Sync to Clip time, where the tape "
	"runs at the baud rate. As a transition, the transition's own position is "
	"the tape: the outgoing shot shows through every address that has not "
	"arrived yet (Background = Clip), and the last frame is the loaded "
	"Spectrum screen, after which the edit cuts to the real picture.\n\n"
	"Note: the Resolume build can also sync the load to the beat or the bar. "
	"OpenFX hosts give a plugin no tempo, so those two Sync modes are absent "
	"here rather than present and doing nothing.\n\n"
	"https://stoatworks-labs.com";

// Script names. Hosts save projects against these, so they are permanent.
constexpr const char* kParamType        = "machine";
constexpr const char* kParamBaud        = "baud";
constexpr const char* kParamInk         = "ink";
constexpr const char* kParamPaper       = "paper";
constexpr const char* kParamBright      = "bright";
constexpr const char* kParamProgress    = "progress";
constexpr const char* kParamSync        = "sync";
constexpr const char* kParamErrorRate   = "errorRate";
constexpr const char* kParamMessage     = "messageOn";
constexpr const char* kParamBorderOn    = "borderOn";
constexpr const char* kParamBorderWidth = "borderWidth";
constexpr const char* kParamPilotLength = "pilotLength";
constexpr const char* kParamMix         = "mix";
constexpr const char* kParamBackground  = "background";

/// The Sync options this build has: the first two of the FFGL build's four, at
/// the same indices.
enum SyncMode
{
	kSyncManual = 0,
	kSyncClip   = 1,
};

using namespace pilot;

//---------------------------------------------------------------------------
// The host's images, as Render.cpp reads them.
//---------------------------------------------------------------------------
render::View viewOf( const OFX::Image* image, bool premultiplied )
{
	render::View view;
	if( image == nullptr )
		return view;

	const OfxRectI b = image->getBounds();
	view.width       = b.x2 - b.x1;
	view.height      = b.y2 - b.y1;
	view.base        = image->getPixelAddress( b.x1, b.y1 );
	view.rowBytes    = image->getRowBytes();
	view.components  = image->getPixelComponents() == OFX::ePixelComponentRGB ? 3 : 4;

	switch( image->getPixelDepth() )
	{
		case OFX::eBitDepthUShort:
			view.depth = render::Depth::U16;
			break;
		case OFX::eBitDepthFloat:
			view.depth = render::Depth::F32;
			break;
		default:
			view.depth = render::Depth::U8;
			break;
	}

	//An RGB clip has no alpha to be premultiplied by, and a host that says
	//"unpremultiplied" about one is describing something that does not exist.
	view.premultiplied = view.components != 4 || premultiplied;
	return view;
}

bool isPremultiplied( OFX::Clip* clip )
{
	return clip == nullptr || clip->getPreMultiplication() != OFX::eImageUnPreMultiplied;
}

//---------------------------------------------------------------------------
/// Pass 1, split across the host's threads by raster row. 49,152 pixels of up
/// to 64 bilinear taps each is the one stage worth spreading out; the attribute
/// pass is 768 cells and runs on the calling thread.
//---------------------------------------------------------------------------
class RasterJob : public OFX::MultiThread::Processor
{
public:
	RasterJob( const render::View& picture, render::Raster& raster ) :
		picture( picture ),
		raster( raster )
	{
	}

	void multiThreadFunction( unsigned int threadID, unsigned int nThreads ) override
	{
		const int n     = static_cast< int >( std::max( nThreads, 1u ) );
		const int id    = static_cast< int >( threadID );
		const int begin = zx::kScreenH * id / n;
		const int end   = zx::kScreenH * ( id + 1 ) / n;
		render::RasterRows( picture, raster, begin, end );
	}

private:
	const render::View& picture;
	render::Raster& raster;
};

//---------------------------------------------------------------------------
/// Pass 3, per output row, on the host's threads, and the conversion out.
//---------------------------------------------------------------------------
class ComposeBase : public OFX::ImageProcessor
{
public:
	explicit ComposeBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setup( const frame::Uniforms& u, const render::Raster& r, const render::Attributes& a,
	            const render::View& under, const OfxRectI& frameBounds, bool outPremultiplied )
	{
		uniforms      = &u;
		raster        = &r;
		attributes    = &a;
		underlay      = under;
		bounds        = frameBounds;
		premultiplied = outPremultiplied;
	}

protected:
	const frame::Uniforms* uniforms      = nullptr;
	const render::Raster* raster         = nullptr;
	const render::Attributes* attributes = nullptr;
	render::View underlay;
	OfxRectI bounds    = { 0, 0, 0, 0 };
	bool premultiplied = true;
};

template< class PIX, int nComponents, int maxValue >
class Compose : public ComposeBase
{
public:
	explicit Compose( OFX::ImageEffect& effect ) :
		ComposeBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const int outW = bounds.x2 - bounds.x1;
		const int outH = bounds.y2 - bounds.y1;
		const int span = window.x2 - window.x1;
		if( span <= 0 )
			return;

		std::vector< float > row( static_cast< size_t >( span ) * 4 );

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			render::ComposeRow( *uniforms, *raster, *attributes, underlay, outW, outH, y - bounds.y1,
			                    window.x1 - bounds.x1, window.x2 - bounds.x1, row.data() );

			PIX* dst = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );
			if( dst == nullptr )
				continue;

			const float* src = row.data();
			for( int x = 0; x < span; ++x, src += 4, dst += nComponents )
			{
				//Render.cpp works premultiplied, as the GPU does. A host that
				//wants straight colour gets alpha divided back out.
				const float a = src[ 3 ];
				for( int c = 0; c < 3; ++c )
				{
					float v = src[ c ];
					if( !premultiplied && nComponents == 4 )
						v = a > 0.0f ? v / a : 0.0f;
					dst[ c ] = quantise( v );
				}
				if( nComponents == 4 )
					dst[ 3 ] = quantise( a );
			}
		}
	}

private:
	/// Integer formats clamp and round; float is left alone, because a float
	/// host may legitimately carry values outside 0..1 in the clip this mixes
	/// against, and the effect's own colours are inside it already.
	static PIX quantise( float v )
	{
		if( maxValue == 1 )
			return static_cast< PIX >( v );
		v = std::clamp( v, 0.0f, 1.0f );
		return static_cast< PIX >( std::lround( v * static_cast< float >( maxValue ) ) );
	}
};

//---------------------------------------------------------------------------
class PilotPlugin : public OFX::ImageEffect
{
public:
	explicit PilotPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		transition = getContext() == OFX::eContextTransition;

		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		if( transition )
		{
			fromClip = fetchClip( kOfxImageEffectTransitionSourceFromClipName );
			toClip   = fetchClip( kOfxImageEffectTransitionSourceToClipName );
			position = fetchDoubleParam( kOfxImageEffectTransitionParamName );
		}
		else
		{
			fromClip = toClip = fetchClip( kOfxImageEffectSimpleSourceClipName );
			progress = fetchDoubleParam( kParamProgress );
			sync     = fetchChoiceParam( kParamSync );
		}

		type        = fetchChoiceParam( kParamType );
		baud        = fetchDoubleParam( kParamBaud );
		ink         = fetchChoiceParam( kParamInk );
		paper       = fetchChoiceParam( kParamPaper );
		bright      = fetchChoiceParam( kParamBright );
		errorRate   = fetchDoubleParam( kParamErrorRate );
		message     = fetchBooleanParam( kParamMessage );
		borderOn    = fetchBooleanParam( kParamBorderOn );
		borderWidth = fetchDoubleParam( kParamBorderWidth );
		pilotLength = fetchDoubleParam( kParamPilotLength );
		mix         = fetchDoubleParam( kParamMix );
		background  = fetchChoiceParam( kParamBackground );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > to( toClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > from;
		if( transition )
			from.reset( fromClip->fetchImage( args.time ) );

		if( dst == nullptr || to == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = dst->getBounds();
		if( bounds.x2 <= bounds.x1 || bounds.y2 <= bounds.y1 )
			return;

		//Every parameter is read here, on the calling thread, before a pixel is
		//touched: OFX forbids reading one during the threaded part of a render.
		const frame::Uniforms u = uniformsAt( args.time );

		const render::View picture = viewOf( to.get(), isPremultiplied( toClip ) );
		//A transition with nothing outgoing -- the head of a timeline, say --
		//loads over the incoming picture, as the filter does.
		const render::View underlay = from != nullptr ? viewOf( from.get(), isPremultiplied( fromClip ) ) : picture;

		render::Raster raster;
		{
			RasterJob job( picture, raster );
			job.multiThread();
		}
		render::Attributes attributes;
		render::Attribute( raster, u.brightMode, attributes );

		const bool outPremultiplied = comps != OFX::ePixelComponentRGBA || isPremultiplied( dstClip );

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				comps == OFX::ePixelComponentRGBA
					? run< Compose< unsigned char, 4, 255 > >( args, dst.get(), u, raster, attributes, underlay, bounds, outPremultiplied )
					: run< Compose< unsigned char, 3, 255 > >( args, dst.get(), u, raster, attributes, underlay, bounds, outPremultiplied );
				break;
			case OFX::eBitDepthUShort:
				comps == OFX::ePixelComponentRGBA
					? run< Compose< unsigned short, 4, 65535 > >( args, dst.get(), u, raster, attributes, underlay, bounds, outPremultiplied )
					: run< Compose< unsigned short, 3, 65535 > >( args, dst.get(), u, raster, attributes, underlay, bounds, outPremultiplied );
				break;
			case OFX::eBitDepthFloat:
				comps == OFX::ePixelComponentRGBA
					? run< Compose< float, 4, 1 > >( args, dst.get(), u, raster, attributes, underlay, bounds, outPremultiplied )
					: run< Compose< float, 3, 1 > >( args, dst.get(), u, raster, attributes, underlay, bounds, outPremultiplied );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime ) override
	{
		//Mix at zero is the untouched input -- the filter's Source, or the
		//transition's outgoing shot, which is what Mix fades against there.
		if( mix->getValueAtTime( args.time ) <= 0.0 )
		{
			identityClip = fromClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		stoatworks::about::ofx::changedParam( args, paramName );
	}

private:
	static int choice( OFX::ChoiceParam* param, double time )
	{
		//ChoiceParam answers through an out parameter rather than a return
		//value, unlike every other param type in the Support library.
		int value = 0;
		param->getValueAtTime( time, value );
		return value;
	}

	frame::Uniforms uniformsAt( double time ) const
	{
		frame::HostValues host;
		host.type        = static_cast< float >( choice( type, time ) );
		host.baud        = static_cast< float >( baud->getValueAtTime( time ) );
		host.ink         = static_cast< float >( choice( ink, time ) );
		host.paper       = static_cast< float >( choice( paper, time ) );
		host.bright      = static_cast< float >( choice( bright, time ) );
		host.errorRate   = static_cast< float >( errorRate->getValueAtTime( time ) );
		host.message     = message->getValueAtTime( time ) ? 1.0f : 0.0f;
		host.borderOn    = borderOn->getValueAtTime( time ) ? 1.0f : 0.0f;
		host.borderWidth = static_cast< float >( borderWidth->getValueAtTime( time ) );
		host.pilotLength = static_cast< float >( pilotLength->getValueAtTime( time ) );
		host.mix         = static_cast< float >( mix->getValueAtTime( time ) );
		host.background  = static_cast< float >( choice( background, time ) );

		//OFX time is FRAMES. The border and Clip time want seconds, from the
		//output's own rate, and a host that reports none -- some do, with
		//nothing connected -- would otherwise divide by it.
		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) && toClip != nullptr )
			fps = toClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 25.0;
		const double seconds = time / fps;

		float effective = 0.0f;
		if( transition )
		{
			//The host's position IS the tape. Clamped, never wrapped: the last
			//frame of a transition is a whole tape, not the start of another.
			effective = std::clamp( static_cast< float >( position->getValueAtTime( time ) ), 0.0f, 1.0f );
		}
		else
		{
			host.progress = static_cast< float >( progress->getValueAtTime( time ) );
			host.sync     = static_cast< float >( choice( sync, time ) );
			effective     = static_cast< int >( host.sync ) == kSyncClip
			                    ? frame::ClipTimeProgress( host, seconds )
			                    : std::clamp( host.progress, 0.0f, 1.0f );
		}

		return frame::Prepare( host, effective, seconds );
	}

	template< class Processor >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, const frame::Uniforms& u,
	          const render::Raster& raster, const render::Attributes& attributes,
	          const render::View& underlay, const OfxRectI& bounds, bool outPremultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setup( u, raster, attributes, underlay, bounds, outPremultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	bool transition = false;

	OFX::Clip* dstClip  = nullptr;
	OFX::Clip* fromClip = nullptr;///< the filter's Source, or SourceFrom
	OFX::Clip* toClip   = nullptr;///< the filter's Source, or SourceTo

	OFX::DoubleParam* position    = nullptr;///< the transition's own, in that context only
	OFX::DoubleParam* progress    = nullptr;///< the filter's, in that context only
	OFX::ChoiceParam* sync        = nullptr;///< ditto
	OFX::ChoiceParam* type        = nullptr;
	OFX::DoubleParam* baud        = nullptr;
	OFX::ChoiceParam* ink         = nullptr;
	OFX::ChoiceParam* paper       = nullptr;
	OFX::ChoiceParam* bright      = nullptr;
	OFX::DoubleParam* errorRate   = nullptr;
	OFX::BooleanParam* message    = nullptr;
	OFX::BooleanParam* borderOn   = nullptr;
	OFX::DoubleParam* borderWidth = nullptr;
	OFX::DoubleParam* pilotLength = nullptr;
	OFX::DoubleParam* mix         = nullptr;
	OFX::ChoiceParam* background  = nullptr;
};

//---------------------------------------------------------------------------
// Description.
//---------------------------------------------------------------------------
OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( name, name, name );
	page->addChild( *group );
	return group;
}

void defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
}

void defineToggle( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, float value )
{
	OFX::BooleanParamDescriptor* param = desc.defineBooleanParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setDefault( value > 0.5f );
	param->setParent( *group );
	page->addChild( *param );
}

OFX::ChoiceParamDescriptor* defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* group, const char* name, const char* label,
                                          const char* hint )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setParent( *group );
	page->addChild( *param );
	return param;
}

int choiceIndex( float value )
{
	return static_cast< int >( std::lround( value ) );
}

mDeclarePluginFactory( PilotPluginFactory, {}, {} );
} // namespace

void PilotPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );
	desc.addSupportedContext( OFX::eContextTransition );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The raster is a box filter of the whole picture, so there is no tile
	// smaller than the frame. Frames are independent of each other and of
	// render order: the tape is a function of the controls and the time.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setSupportsMultipleClipPARs( false );
	desc.setSupportsMultipleClipDepths( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void PilotPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context )
{
	const bool transition = context == OFX::eContextTransition;

	const auto defineInput = [ &desc ]( const char* name ) {
		OFX::ClipDescriptor* clip = desc.defineClip( name );
		clip->addSupportedComponent( OFX::ePixelComponentRGBA );
		clip->addSupportedComponent( OFX::ePixelComponentRGB );
		clip->setTemporalClipAccess( false );
		clip->setSupportsTiles( false );
	};

	if( transition )
	{
		defineInput( kOfxImageEffectTransitionSourceFromClipName );
		defineInput( kOfxImageEffectTransitionSourceToClipName );
	}
	else
	{
		defineInput( kOfxImageEffectSimpleSourceClipName );
	}

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	if( transition )
	{
		// The mandated position, 0 to 1. The host draws and animates it; it is
		// on no page because it is not the plugin's to show.
		OFX::DoubleParamDescriptor* position = desc.defineDoubleParam( kOfxImageEffectTransitionParamName );
		position->setLabels( "Transition", "Transition", "Transition" );
		position->setRange( 0.0, 1.0 );
		position->setDisplayRange( 0.0, 1.0 );
		position->setDefault( 0.0 );
	}

	// Same names, same 0..1 ranges, same defaults and the same four groups as
	// the FFGL build -- the defaults are literally the same struct -- so the two
	// inspectors read alike and one guide covers both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const frame::HostValues defaults;

	//---------------------------------------------------------------- Machine
	OFX::GroupParamDescriptor* machineGroup = defineGroup( desc, page, "Machine" );

	OFX::ChoiceParamDescriptor* typeParam = defineChoice(
		desc, page, machineGroup, kParamType, "Type",
		"The loader: its baud rate, its border colours and how its border is painted. Only ZX 48 "
		"models a documented routine; the other three are loaders in the same shape." );
	for( int i = 0; i < machineCount(); ++i )
		typeParam->appendOption( machine( i ).name );
	typeParam->setDefault( choiceIndex( defaults.type ) );

	defineSlider( desc, page, machineGroup, kParamBaud, "Baud",
	              "A trim around the machine's own rate: x0.25 at 0, x1 at the centre, x4 at the top. "
	              "Sets the border's stripe rate and the Clip time period. It does not drive the reveal "
	              "under Manual sync, where Progress is the position.",
	              defaults.baud );

	OFX::ChoiceParamDescriptor* inkParam = defineChoice(
		desc, page, machineGroup, kParamInk, "Ink",
		"The power-on attribute's ink: what a cell's dark pixels show before its own colours arrive." );
	OFX::ChoiceParamDescriptor* paperParam = defineChoice(
		desc, page, machineGroup, kParamPaper, "Paper",
		"The power-on attribute's paper, and what an address that has not arrived shows under "
		"Background = Paper. A Spectrum powers up black on white." );
	for( int i = 0; i < zx::kColourCount; ++i )
	{
		inkParam->appendOption( zx::ColourNames()[ i ] );
		paperParam->appendOption( zx::ColourNames()[ i ] );
	}
	inkParam->setDefault( choiceIndex( defaults.ink ) );
	paperParam->setDefault( choiceIndex( defaults.paper ) );

	OFX::ChoiceParamDescriptor* brightParam = defineChoice(
		desc, page, machineGroup, kParamBright, "Bright",
		"BRIGHT is one bit per cell. Auto lets each cell's brightest pixel decide; On also brightens "
		"the power-on attribute." );
	brightParam->appendOption( "Off" );
	brightParam->appendOption( "Auto" );
	brightParam->appendOption( "On" );
	brightParam->setDefault( choiceIndex( defaults.bright ) );

	//------------------------------------------------------------------- Load
	OFX::GroupParamDescriptor* loadGroup = defineGroup( desc, page, "Load" );

	if( !transition )
	{
		defineSlider( desc, page, loadGroup, kParamProgress, "Progress",
		              "How far through the tape. The first Pilot Length of the range is pilot tone, "
		              "with nothing on screen yet; colour arrives over the last 11%. Keyframe it, or set "
		              "Sync to Clip time.",
		              defaults.progress );

		OFX::ChoiceParamDescriptor* syncParam = defineChoice(
			desc, page, loadGroup, kParamSync, "Sync",
			"Manual: Progress is the position. Clip time: the tape runs at the baud rate, offset by "
			"Progress, and starts again when it ends. (The Resolume build's Beat and Bar modes are not "
			"here: an OpenFX host gives a plugin no tempo.)" );
		syncParam->appendOption( "Manual" );
		syncParam->appendOption( "Clip time" );
		syncParam->setDefault( choiceIndex( defaults.sync ) );
	}

	defineSlider( desc, page, loadGroup, kParamErrorRate, "Error Rate",
	              "The chance each 256-byte block fails to read. A failure holds the picture under the "
	              "Spectrum's own report, then the load starts again from the first block.",
	              defaults.errorRate );

	defineToggle( desc, page, loadGroup, kParamMessage, "Message On",
	              "Print 'R Tape loading error, 0:1' while a failed block's report is up.", defaults.message );

	//----------------------------------------------------------------- Border
	OFX::GroupParamDescriptor* borderGroup = defineGroup( desc, page, "Border" );

	defineToggle( desc, page, borderGroup, kParamBorderOn, "Border On",
	              "Off is not a colour, it is no border: the screen fills the frame.", defaults.borderOn );

	defineSlider( desc, page, borderGroup, kParamBorderWidth, "Border Width",
	              "How much of the frame is border, up to a quarter off each edge.", defaults.borderWidth );

	defineSlider( desc, page, borderGroup, kParamPilotLength, "Pilot Length",
	              "How much of the load is spent on the pilot tone -- border stripes, nothing on screen "
	              "-- before the first byte lands.",
	              defaults.pilotLength );

	//----------------------------------------------------------------- Output
	OFX::GroupParamDescriptor* outputGroup = defineGroup( desc, page, "Output" );

	defineSlider( desc, page, outputGroup, kParamMix, "Mix",
	              transition ? "Wet/dry against the outgoing shot. 0 holds the outgoing shot untouched."
	                         : "Wet/dry against the untouched clip. 0 is a bypass.",
	              defaults.mix );

	OFX::ChoiceParamDescriptor* backgroundParam = defineChoice(
		desc, page, outputGroup, kParamBackground, "Background",
		transition ? "What an address that has not arrived shows. Clip is the outgoing shot, so the new "
		             "picture loads over the old one; Paper is what a real machine shows."
		           : "What an address that has not arrived shows. Paper is what a real machine shows; "
		             "Clip shows the clip itself, so the load quantises it as it goes." );
	backgroundParam->appendOption( "Paper" );
	backgroundParam->appendOption( "Black" );
	backgroundParam->appendOption( "Clip" );
	// In a transition the outgoing shot is only ever seen through Clip, so that
	// is where the transition starts. The filter keeps the FFGL build's Paper.
	backgroundParam->setDefault( transition ? frame::kBackgroundClip : choiceIndex( defaults.background ) );

	// The Stoatworks About block: a read-only credit line and one push button per
	// link, in a group that starts folded. Last, so it sits under the effect's
	// own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* PilotPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new PilotPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static PilotPluginFactory* factory =
		new PilotPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
