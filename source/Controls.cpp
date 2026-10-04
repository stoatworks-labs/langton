#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace langton
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
constexpr double kSpeedLow = 1.0, kSpeedHigh = 1.0e6;
constexpr double kBurstLow = 10.0, kBurstHigh = 1.0e5;
constexpr double kTrailLow = 10.0, kTrailHigh = 1.0e6;
} // namespace

const std::vector< unsigned int >& HostOrder( bool effect )
{
	auto build = []( bool over ) {
		std::vector< unsigned int > order = { PT_RULE,    PT_ANTS,    PT_SPEED, PT_REVERSE,     PT_STEP,
			                                  PT_PATTERN, PT_DENSITY, PT_SEED,  PT_RESEED,      PT_AUDIO,
			                                  PT_AUDIO_BURST, PT_AUDIO_REVERSE, PT_CELL_SIZE, PT_GAP,
			                                  PT_PALETTE, PT_TRAIL,   PT_SHOW_ANTS };
		if( over )
			order.insert( order.end(), { PT_CLIP_COLOUR, PT_BACKDROP, PT_MIX } );
		for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
			order.push_back( id );
		return order;
	};
	static const std::vector< unsigned int > source = build( false ), over = build( true );
	return effect ? over : source;
}

const char* GroupOf( unsigned int id )
{
	if( id <= PT_ANTS )
		return "Automaton";
	if( id <= PT_STEP )
		return "Time";
	if( id <= PT_RESEED )
		return "Seeding";
	if( id <= PT_AUDIO_REVERSE )
		return "Audio";
	if( id <= PT_MIX )
		return "Look";
	return "About";
}

const char* NameOf( unsigned int id )
{
	switch( id )
	{
	case PT_RULE: return "Rule";
	case PT_ANTS: return "Ants";
	case PT_SPEED: return "Speed";
	case PT_REVERSE: return "Reverse";
	case PT_STEP: return "Step";
	case PT_PATTERN: return "Pattern";
	case PT_DENSITY: return "Density";
	case PT_SEED: return "Seed";
	case PT_RESEED: return "Reseed";
	case PT_AUDIO: return "Audio";
	case PT_AUDIO_BURST: return "Audio Burst";
	case PT_AUDIO_REVERSE: return "Audio Reverse";
	case PT_CELL_SIZE: return "Cell Size";
	case PT_GAP: return "Gap";
	case PT_PALETTE: return "Palette";
	case PT_TRAIL: return "Trail";
	case PT_SHOW_ANTS: return "Show Ants";
	case PT_CLIP_COLOUR: return "Clip Colour";
	case PT_BACKDROP: return "Backdrop";
	case PT_MIX: return "Mix";
	default: return "?";
	}
}

int PatternCount( bool effect )
{
	return effect ? static_cast< int >( Pattern::Count ) : static_cast< int >( Pattern::Clip );
}

const char* PatternName( Pattern pattern )
{
	switch( pattern )
	{
	case Pattern::Empty: return "Empty";
	case Pattern::Soup: return "Soup";
	case Pattern::Clip: return "Clip";
	default: return "?";
	}
}

const char* PaletteName( Palette palette )
{
	switch( palette )
	{
	case Palette::Phosphor: return "Phosphor";
	case Palette::Heat: return "Heat";
	case Palette::Ice: return "Ice";
	case Palette::Mono: return "Mono";
	case Palette::Spectrum: return "Spectrum";
	default: return "?";
	}
}

double SpeedFromParam( float v )
{
	//Exactly zero at the bottom: the ants stand still, and Step, Audio Burst
	//and Reverse still work by hand.
	if( v <= 0.0f )
		return 0.0;
	return geometric( v, kSpeedLow, kSpeedHigh );
}
float ParamFromSpeed( double roundsPerSecond )
{
	return roundsPerSecond <= 0.0 ? 0.0f : inverseGeometric( roundsPerSecond, kSpeedLow, kSpeedHigh );
}
double DensityFromParam( float v )
{
	return clamp01( v );
}
long long BurstFromParam( float v )
{
	if( v <= 0.0f )
		return 0;
	return std::llround( geometric( v, kBurstLow, kBurstHigh ) );
}
int RowsFromParam( float v )
{
	return static_cast< int >( std::lround( geometric( v, kMaxRows, kMinRows ) ) );
}
float ParamFromRows( int rows )
{
	return inverseGeometric( rows, kMaxRows, kMinRows );
}
double GapFromParam( float v )
{
	return 0.5 * clamp01( v );
}
double TrailFromParam( float v )
{
	if( v <= 0.0f )
		return 0.0;
	return geometric( v, kTrailLow, kTrailHigh );
}
float ParamFromTrail( double steps )
{
	return steps <= 0.0 ? 0.0f : inverseGeometric( steps, kTrailLow, kTrailHigh );
}

uint32_t ThresholdU32( double probability )
{
	if( !( probability > 0.0 ) )
		return 0u;
	const double scaled = std::round( probability * 4294967296.0 );
	return scaled >= 4294967295.0 ? 0xFFFFFFFFu : static_cast< uint32_t >( scaled );
}

Grid GridFor( int rows, int width, int height )
{
	Grid grid;
	grid.rows = std::max( rows, 1 );
	const double aspect = height > 0 ? static_cast< double >( width ) / height : 1.0;
	grid.cols = std::max( 1, static_cast< int >( std::lround( grid.rows * aspect ) ) );
	return grid;
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace langton
