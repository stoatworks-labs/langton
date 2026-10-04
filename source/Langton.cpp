#include "Langton.h"

#include "Diag.h"
#include "GLState.h"
#include "Hash.h"
#include "Shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;

namespace langton
{
namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

constexpr int kClockVotes = 4;
/// Host seconds. A bigger forward step, or any backward one, is a jump -- a
/// clip trigger or a scrub -- and no time passes across it.
constexpr double kMaxFrameDelta = 1.0;

struct Look
{
	float first[ 3 ], last[ 3 ], trail[ 3 ], ant[ 3 ];
};

const Look& lookOf( Palette palette )
{
	//Linear light. Colour 1 is `first`, the rule's last colour `last`, the
	//ones between evenly between; Spectrum turns the hue instead. The ant is
	//a colour none of its cells can be.
	static const Look looks[] = {
		{ { 0.80f, 1.00f, 0.80f }, { 0.05f, 0.50f, 0.15f }, { 0.20f, 0.70f, 0.30f }, { 1.00f, 0.30f, 0.15f } },//Phosphor
		{ { 1.00f, 0.95f, 0.70f }, { 0.80f, 0.12f, 0.02f }, { 0.70f, 0.25f, 0.05f }, { 0.35f, 0.80f, 1.00f } },//Heat
		{ { 0.92f, 0.98f, 1.00f }, { 0.10f, 0.35f, 0.95f }, { 0.20f, 0.45f, 0.90f }, { 1.00f, 0.60f, 0.15f } },//Ice
		{ { 1.00f, 1.00f, 1.00f }, { 1.00f, 1.00f, 1.00f }, { 0.50f, 0.50f, 0.50f }, { 1.00f, 0.20f, 0.20f } },//Mono
		{ { 1.00f, 1.00f, 1.00f }, { 1.00f, 1.00f, 1.00f }, { 0.35f, 0.35f, 0.40f }, { 1.00f, 1.00f, 1.00f } },//Spectrum
	};
	return looks[ static_cast< int >( palette ) ];
}

/// A hue wheel from clamped triangles (no trig), the composite's own shape.
void hueOf( double h, float out[ 3 ] )
{
	const double offsets[ 3 ] = { 0.0, 2.0 / 3.0, 1.0 / 3.0 };
	for( int i = 0; i < 3; ++i )
	{
		double f = h + offsets[ i ];
		f -= std::floor( f );
		out[ i ] = static_cast< float >( std::clamp( std::fabs( f * 6.0 - 3.0 ) - 1.0, 0.0, 1.0 ) );
	}
}

void setUint( FFGLShader& shader, const char* name, uint32_t value )
{
	glUniform1ui( glGetUniformLocation( shader.GetGLID(), name ), value );
}

void setIvec2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( glGetUniformLocation( shader.GetGLID(), name ), x, y );
}
} // namespace

static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
LangtonPlugin::LangtonPlugin( bool effect ) :
	isEffect( effect ),
	hostOrder( HostOrder( effect ) )
{
	SetMinInputs( isEffect ? 1 : 0 );
	SetMaxInputs( isEffect ? 1 : 0 );
	SetTimeSupported( true );

	std::fill( std::begin( idToHost ), std::end( idToHost ), -1 );
	for( size_t i = 0; i < hostOrder.size(); ++i )
		idToHost[ hostOrder[ i ] ] = static_cast< int >( i );

	//-------------------------------------------------------------------
	// Defaults. The source: Langton's own ant on an empty grid, 600 steps a
	// second, so the highway comes out of the chaos after about seventeen
	// seconds. The Over: four ants on the clip's picture, faster.
	//-------------------------------------------------------------------
	params[ PT_RULE ]          = 0.0f;
	params[ PT_ANTS ]          = isEffect ? 4.0f : 1.0f;
	params[ PT_SPEED ]         = ParamFromSpeed( isEffect ? 3000.0 : 600.0 );
	params[ PT_REVERSE ]       = 0.0f;
	params[ PT_PATTERN ]       = static_cast< float >( isEffect ? Pattern::Clip : Pattern::Empty );
	params[ PT_DENSITY ]       = 0.2f;
	params[ PT_SEED ]          = 1.0f;
	params[ PT_AUDIO_BURST ]   = 0.0f;
	params[ PT_AUDIO_REVERSE ] = 0.0f;
	params[ PT_CELL_SIZE ]     = ParamFromRows( 120 );
	params[ PT_GAP ]           = 0.2f;
	params[ PT_PALETTE ]       = static_cast< float >( Palette::Phosphor );
	params[ PT_TRAIL ]         = ParamFromTrail( 3000.0 );
	params[ PT_SHOW_ANTS ]     = 1.0f;
	params[ PT_CLIP_COLOUR ]   = 0.0f;
	params[ PT_BACKDROP ]      = 0.35f;
	params[ PT_MIX ]           = 1.0f;

	for( unsigned int host = 0; host < hostOrder.size(); ++host )
	{
		const unsigned int id = hostOrder[ host ];
		const char* name      = NameOf( id );
		switch( id )
		{
		case PT_RULE:
			SetOptionParamInfo( host, name, ant::kRuleCount, params[ id ] );
			for( int i = 0; i < ant::kRuleCount; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), ant::kRules[ i ].name, static_cast< float >( i ) );
			break;
		case PT_PATTERN:
		{
			const int count = PatternCount( isEffect );
			SetOptionParamInfo( host, name, static_cast< unsigned int >( count ), params[ id ] );
			for( int i = 0; i < count; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), PatternName( static_cast< Pattern >( i ) ),
				                     static_cast< float >( i ) );
			break;
		}
		case PT_PALETTE:
		{
			const int count = static_cast< int >( Palette::Count );
			SetOptionParamInfo( host, name, static_cast< unsigned int >( count ), params[ id ] );
			for( int i = 0; i < count; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), PaletteName( static_cast< Palette >( i ) ),
				                     static_cast< float >( i ) );
			break;
		}
		case PT_ANTS:
			//Only FF_TYPE_STANDARD has its default clamped into 0..1, so an
			//integer is declared with its real default and range.
			SetParamInfo( host, name, FF_TYPE_INTEGER, params[ id ] );
			SetParamRange( host, 1.0f, static_cast< float >( ant::kMaxAnts ) );
			break;
		case PT_SEED:
			SetParamInfo( host, name, FF_TYPE_INTEGER, params[ id ] );
			SetParamRange( host, 0.0f, 9999.0f );
			break;
		case PT_STEP:
		case PT_RESEED:
			SetParamInfo( host, name, FF_TYPE_EVENT, false );
			break;
		case PT_REVERSE:
		case PT_AUDIO_REVERSE:
		case PT_SHOW_ANTS:
			SetParamInfo( host, name, FF_TYPE_BOOLEAN, params[ id ] > 0.5f );
			break;
		case PT_AUDIO:
			//An FFT buffer: Resolume shows it as an audio-source picker.
			SetBufferParamInfo( host, name, audio::kBins, FF_USAGE_FFT );
			for( int i = 0; i < audio::kBins; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), "", 0.0f );
			break;
		case PT_ABOUT_TEXT:
			SetParamInfo( host, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
			break;
		default:
			if( id > PT_ABOUT_TEXT )
				SetParamInfo( host, stoatworks::about::buttons()[ id - PT_ABOUT_TEXT - 1 ].label, FF_TYPE_EVENT, false );
			else
				SetParamInfo( host, name, FF_TYPE_STANDARD, params[ id ] );
			break;
		}
		SetParamGroup( host, GroupOf( id ) );
	}
}

LangtonPlugin::~LangtonPlugin() = default;

//---------------------------------------------------------------------------
FFResult LangtonPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	using namespace shaders;
	const std::string quadVertex = std::string( kVersion ) + kQuadVertex;
	struct Stage
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &sampleShader, Assemble( kSampleFragment ), "sample" },
		{ &compositeShader, Assemble( kCompositeFragment ), "composite" },
	};
	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( quadVertex.c_str(), stage.fragment.c_str() ) )
			continue;
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Langton: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//A sampler bound to texture 0 is "unloadable" to Apple's GL (boreal's
	//trap): the source has no clip, so its clip sampler gets this.
	glGenTextures( 1, &blankClip );
	glBindTexture( GL_TEXTURE_2D, blankClip );
	const unsigned char black[ 4 ] = { 0, 0, 0, 0 };
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	diag::info( isEffect ? "initialised (Over)" : "initialised (source)" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void LangtonPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;
	const double raw = hostTime;

	//Resolume has been seen sending seconds and milliseconds through SetTime:
	//vote on the unit against the wall clock (rosette's code, via radar).
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale  = millisVotes > secondsVotes ? 0.001 : 1.0;
				settledJump = true;
			}
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;
	now          = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

uint32_t LangtonPlugin::seedSalt() const
{
	const uint32_t seedValue = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	return Pcg( seedValue * 2654435761u ^ Pcg( seedSerial + 0x9e3779b9u ) );
}

//---------------------------------------------------------------------------
// A fresh field: the rule, the ants at their start places, the ground.
//---------------------------------------------------------------------------
void LangtonPlugin::seed( const FFGLTextureStruct* input, int width, int height )
{
	const int ruleIndex = OptionIndex( params[ PT_RULE ], ant::kRuleCount );
	world.Reset( grid.cols, grid.rows );
	world.SetRule( ant::kRules[ ruleIndex ].turns );
	world.PlaceAnts( std::clamp( static_cast< int >( std::lround( params[ PT_ANTS ] ) ), 1, ant::kMaxAnts ) );
	const Pattern pattern = static_cast< Pattern >( OptionIndex( params[ PT_PATTERN ], PatternCount( isEffect ) ) );
	if( pattern == Pattern::Soup )
		world.Soup( ThresholdU32( DensityFromParam( params[ PT_DENSITY ] ) ), seedSalt() );
	else if( pattern == Pattern::Clip && input != nullptr )
	{
		//The clip, a level per cell, read back for the walk: once a reseed.
		sampleTarget.BindForDrawing();
		{
			ScopedShaderBinding shader( sampleShader.GetGLID() );
			bindUnit( 0, input->Handle );
			sampleShader.Set( "Clip", 0 );
			setIvec2( sampleShader, "ClipRaster", width, height );
			setIvec2( sampleShader, "Grid", grid.cols, grid.rows );
			sampleShader.Set( "Colours", world.Colours() );
			sampleShader.Set( "MirrorForTest", mirrorForTest ? 1 : 0 );
			quad.Draw();
			unbindTextureUnits( 1 );
		}
		std::vector< uint32_t > levels( static_cast< size_t >( grid.cols ) * grid.rows );
		GLint alignment = 4;
		glGetIntegerv( GL_PACK_ALIGNMENT, &alignment );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, grid.cols, grid.rows, GL_RED_INTEGER, GL_UNSIGNED_INT, levels.data() );
		glPixelStorei( GL_PACK_ALIGNMENT, alignment );
		std::vector< uint8_t > colours( levels.size() );
		for( size_t i = 0; i < levels.size(); ++i )
			colours[ i ] = static_cast< uint8_t >( levels[ i ] );
		world.Load( colours );
	}
	lastRule = ruleIndex;
	lastAnts = static_cast< int >( world.Ants().size() );
	++reseeds;
}

//---------------------------------------------------------------------------
// The rows the walk touched, into the state texture.
//---------------------------------------------------------------------------
void LangtonPlugin::upload()
{
	int first = 0, last = 0;
	if( !world.TakeDirtyRows( first, last ) )
		return;
	if( dropLastRowForTest && last > first )
		--last;
	const int count = last - first + 1;
	const int cols  = world.Cols();
	staging.resize( static_cast< size_t >( count ) * cols * 2 );
	const auto& colour  = world.Grid();
	const auto& visited = world.Visited();
	const auto& visit   = world.Visits();
	for( int y = 0; y < count; ++y )
		for( int x = 0; x < cols; ++x )
		{
			const size_t from = static_cast< size_t >( first + y ) * cols + x;
			const size_t to   = ( static_cast< size_t >( y ) * cols + x ) * 2;
			staging[ to ]     = colour[ from ] | ( visited[ from ] ? 0x100u : 0u );
			staging[ to + 1 ] = visit[ from ];
		}
	state.UploadRows( first, count, staging.data() );
}

//---------------------------------------------------------------------------
FFResult LangtonPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct* input = nullptr;
	if( isEffect )
	{
		if( pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		input = pgl->inputTextures[ 0 ];
	}

	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	const int width           = input ? static_cast< int >( input->Width ) : hostViewport[ 2 ];
	const int height          = input ? static_cast< int >( input->Height ) : hostViewport[ 3 ];
	if( width <= 0 || height <= 0 )
		return FF_FAIL;
	glDisable( GL_BLEND );

	//-------------------------------------------------------------------
	// Time: the host's, as real elapsed seconds, frame to frame, in double.
	//-------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	double dt = 0.0;
	if( lastNow >= 0.0 )
	{
		const double stepTime = floatClockForTest
		                            ? static_cast< double >( static_cast< float >( now ) - static_cast< float >( lastNow ) )
		                            : now - lastNow;
		if( stepTime < 0.0 || stepTime > kMaxFrameDelta )
			analyser.Reset();
		else
			dt = stepTime;
	}
	lastNow = now;
	frameDt = dt;

	//-------------------------------------------------------------------
	// Audio: an onset is a burst of rounds, and with Audio Reverse it turns
	// time round.
	//-------------------------------------------------------------------
	{
		float bins[ audio::kBins ] = {};
		int binCount               = 0;
		if( const ParamInfo* info = FindParamInfo( static_cast< unsigned int >( idToHost[ PT_AUDIO ] ) ) )
		{
			binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
			for( int i = 0; i < binCount; ++i )
				bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
		}
		audio::Settings settings;
		analyser.SetPrimingForTest( !unprimed );
		analyser.Update( bins, binCount, static_cast< float >( dt ), settings );
		const long long burst  = BurstFromParam( params[ PT_AUDIO_BURST ] );
		const bool turnRound   = params[ PT_AUDIO_REVERSE ] > 0.5f;
		if( analyser.Fired() && ( burst > 0 || turnRound ) )
		{
			++onsetsUsed;
			pendingRounds += burst;
			if( turnRound )
				audioReversed = !audioReversed;
		}
	}

	//-------------------------------------------------------------------
	// The buttons (a press is the rising edge), and what reseeds. A new Seed
	// starts its sequence of soups again before a Reseed press is read, so a
	// press in the same frame takes the next one (conway's order).
	//-------------------------------------------------------------------
	{
		const bool stepDown = params[ PT_STEP ] > 0.5f;
		if( stepDown && !stepHeld )
			++pendingRounds;
		stepHeld = stepDown;
		const int seedValue = static_cast< int >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
		if( seedValue != lastSeed )
		{
			seedSerial = 0;
			needSeed   = true;
		}
		lastSeed              = seedValue;
		const bool reseedDown = params[ PT_RESEED ] > 0.5f;
		if( reseedDown && !reseedHeld )
		{
			++seedSerial;
			needSeed = true;
		}
		reseedHeld        = reseedDown;
		const int pattern = OptionIndex( params[ PT_PATTERN ], PatternCount( isEffect ) );
		if( pattern != lastPattern )
			needSeed = true;
		lastPattern = pattern;
	}

	//-------------------------------------------------------------------
	// The grid follows Cell Size and the raster's ASPECT. A new size keeps
	// the overlap and the ants, centred.
	//-------------------------------------------------------------------
	const Grid want = gridOverride.cols > 0 ? gridOverride : GridFor( RowsFromParam( params[ PT_CELL_SIZE ] ), width, height );
	if( world.Cols() == 0 )
		needSeed = true;
	else if( want != grid )
		world.Regrid( want.cols, want.rows );
	grid = want;
	if( ( width != lastWidth || height != lastHeight ) && lastWidth != 0 && clearOnResizeForTest )
	{
		world.Reset( grid.cols, grid.rows );
		world.PlaceAnts( static_cast< int >( world.Ants().size() ) );
	}
	lastWidth  = width;
	lastHeight = height;

	//-------------------------------------------------------------------
	// Everything allocated before anything is bound.
	//-------------------------------------------------------------------
	//A new texture is cleared, so all of the walk goes up into it.
	const bool reallocated = state.Width() != grid.cols || state.Height() != grid.rows;
	bool ok                = state.Ensure( grid.cols, grid.rows, GL_RG32UI );
	if( isEffect )
		ok = ok && sampleTarget.Ensure( grid.cols, grid.rows, GL_R32UI );
	if( !ok )
	{
		diag::error( "could not allocate the state buffers" );
		return FF_FAIL;
	}
	if( reallocated )
		world.MarkAllDirty();

	if( needSeed )
	{
		needSeed = false;
		seed( input, width, height );
		world.MarkAllDirty();
	}
	if( antsForTest )
	{
		world.SetAnts( pendingAnts );
		antsForTest = false;
		lastAnts    = static_cast< int >( pendingAnts.size() );
	}

	//-------------------------------------------------------------------
	// A Rule change carries the field on under the new rule; an Ants change
	// puts the ants back at their start places on the same field.
	//-------------------------------------------------------------------
	{
		const int ruleIndex = OptionIndex( params[ PT_RULE ], ant::kRuleCount );
		if( ruleIndex != lastRule )
		{
			world.SetRule( ant::kRules[ ruleIndex ].turns );
			lastRule = ruleIndex;
		}
		const int antCount = std::clamp( static_cast< int >( std::lround( params[ PT_ANTS ] ) ), 1, ant::kMaxAnts );
		if( antCount != lastAnts )
		{
			world.PlaceAnts( antCount );
			lastAnts = antCount;
		}
	}

	//-------------------------------------------------------------------
	// The walk: what the clock owes, then the presses and onsets, at most
	// kMaxRoundsPerFrame; the harness's on top. Reverse and the audio's turn
	// combine: either one alone runs time backwards.
	//-------------------------------------------------------------------
	const double speed     = SpeedFromParam( params[ PT_SPEED ] );
	const long long owed   = clock.Advance( speed, dt, kMaxRoundsPerFrame );
	const long long rounds = std::min( owed + pendingRounds, kMaxRoundsPerFrame ) + testRounds;
	pendingRounds          = 0;
	testRounds             = 0;
	reversedNow            = ( params[ PT_REVERSE ] > 0.5f ) != audioReversed;
	world.Step( rounds, reversedNow );

	upload();
	composite( input, hostViewport, pgl->HostFBO, width, height );
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void LangtonPlugin::composite( const FFGLTextureStruct* input, const GLint* hostViewport, GLuint hostFBO, int width,
                               int height )
{
	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	const Palette palette = static_cast< Palette >( OptionIndex( params[ PT_PALETTE ], static_cast< int >( Palette::Count ) ) );
	const Look& look      = lookOf( palette );

	//The rule's colours: 1 is `first`, n - 1 is `last` (Spectrum: the hue
	//wheel), 0 is never drawn.
	float colours[ ant::kMaxColours * 3 ] = {};
	const int n = std::max( world.Colours(), 2 );
	for( int k = 1; k < n; ++k )
	{
		float* c = &colours[ k * 3 ];
		if( palette == Palette::Spectrum )
			hueOf( n > 2 ? static_cast< double >( k - 1 ) / ( n - 1 ) : 0.0, c );
		else
		{
			const float t = n > 2 ? static_cast< float >( k - 1 ) / static_cast< float >( n - 2 ) : 0.0f;
			for( int i = 0; i < 3; ++i )
				c[ i ] = look.first[ i ] + ( look.last[ i ] - look.first[ i ] ) * t;
		}
	}

	int antPlaces[ ant::kMaxAnts * 2 ] = {};
	const auto& ants   = world.Ants();
	const int antCount = params[ PT_SHOW_ANTS ] > 0.5f ? static_cast< int >( std::min< size_t >( ants.size(), ant::kMaxAnts ) ) : 0;
	for( int i = 0; i < antCount; ++i )
	{
		antPlaces[ i * 2 ]     = ants[ static_cast< size_t >( i ) ].x;
		antPlaces[ i * 2 + 1 ] = ants[ static_cast< size_t >( i ) ].y;
	}

	ScopedShaderBinding shader( compositeShader.GetGLID() );
	bindUnit( 0, state.TextureID() );
	bindUnit( 1, input ? input->Handle : blankClip );
	compositeShader.Set( "State", 0 );
	compositeShader.Set( "InputTexture", 1 );
	setIvec2( compositeShader, "Grid", grid.cols, grid.rows );
	compositeShader.Set( "Raster", static_cast< float >( width ), static_cast< float >( height ) );
	compositeShader.Set( "ViewOrigin", static_cast< float >( hostViewport[ 0 ] ), static_cast< float >( hostViewport[ 1 ] ) );
	compositeShader.Set( "Gap", static_cast< float >( GapFromParam( params[ PT_GAP ] ) ) );
	setUint( compositeShader, "Now", static_cast< uint32_t >( world.Clock() ) );
	compositeShader.Set( "Trail", static_cast< float >( TrailFromParam( params[ PT_TRAIL ] ) ) );
	glUniform3fv( glGetUniformLocation( compositeShader.GetGLID(), "Palette" ), ant::kMaxColours, colours );
	compositeShader.Set( "TrailColour", look.trail[ 0 ], look.trail[ 1 ], look.trail[ 2 ] );
	compositeShader.Set( "AntColour", look.ant[ 0 ], look.ant[ 1 ], look.ant[ 2 ] );
	glUniform2iv( glGetUniformLocation( compositeShader.GetGLID(), "Ants" ), ant::kMaxAnts, antPlaces );
	compositeShader.Set( "AntCount", antCount );
	compositeShader.Set( "IsEffect", isEffect ? 1 : 0 );
	compositeShader.Set( "ClipColour", isEffect ? std::clamp( params[ PT_CLIP_COLOUR ], 0.0f, 1.0f ) : 0.0f );
	compositeShader.Set( "Backdrop", isEffect ? std::clamp( params[ PT_BACKDROP ], 0.0f, 1.0f ) : 0.0f );
	compositeShader.Set( "MixAmount", isEffect ? std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) : 1.0f );
	compositeShader.Set( "PointSampleForTest", pointSampleForTest ? 1 : 0 );
	quad.Draw();
	unbindTextureUnits( 2 );
}

//---------------------------------------------------------------------------
FFResult LangtonPlugin::DeInitGL()
{
	for( FFGLShader* shader : { &sampleShader, &compositeShader } )
		shader->FreeGLResources();
	quad.Release();
	state.Destroy();
	sampleTarget.Destroy();
	if( blankClip )
	{
		glDeleteTextures( 1, &blankClip );
		blankClip = 0;
	}
	//The walk is CPU state and survives; the texture is rebuilt whole.
	world.MarkAllDirty();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult LangtonPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

char* LangtonPlugin::GetTextParameter( unsigned int index )
{
	if( index < hostOrder.size() && hostOrder[ index ] == PT_ABOUT_TEXT )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult LangtonPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index < hostOrder.size() && hostOrder[ index ] == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult LangtonPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= hostOrder.size() )
		return FF_FAIL;
	const unsigned int id = hostOrder[ index ];
	if( id >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( id - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	params[ id ] = value;
	return FF_SUCCESS;
}

float LangtonPlugin::GetFloatParameter( unsigned int index )
{
	return index < hostOrder.size() ? params[ hostOrder[ index ] ] : 0.0f;
}

void LangtonPlugin::SetById( unsigned int id, float value )
{
	const int host = HostIndexOf( id );
	if( host >= 0 )
		SetFloatParameter( static_cast< unsigned int >( host ), value );
}

} // namespace langton
