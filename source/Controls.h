#pragma once

#include "StoatworksAboutLinks.h"

#include <cstdint>
#include <vector>

/**
    The host's parameters, and what they mean in the walk's units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange` could
    widen it. The conversions live in Controls.cpp, one function per control.
    Option, boolean and event parameters hold the element value itself; Ants
    and Seed are real integers (`FF_TYPE_INTEGER`).

    Units: steps (one round: every ant one step), steps per second, cells
    (rows of the grid), and probabilities as 32-bit integer thresholds.
*/
namespace langton
{
/**
    Every control either plugin has, by what it is -- NOT by the index a host
    sees. Each plugin declares its own subset, densely, in the order of
    `HostOrder()`, with the About block LAST in both (radar's and conway's
    shape: a future User guide button moves no control).
*/
enum ParamId : unsigned int
{
	// -- Automaton -----------------------------------------------------------
	PT_RULE = 0,
	PT_ANTS,

	// -- Time ----------------------------------------------------------------
	PT_SPEED,
	PT_REVERSE,
	PT_STEP,

	// -- Seeding -------------------------------------------------------------
	PT_PATTERN,
	PT_DENSITY,
	PT_SEED,
	PT_RESEED,

	// -- Audio ---------------------------------------------------------------
	PT_AUDIO,
	PT_AUDIO_BURST,
	PT_AUDIO_REVERSE,

	// -- Look ----------------------------------------------------------------
	PT_CELL_SIZE,
	PT_GAP,
	PT_PALETTE,
	PT_TRAIL,
	PT_SHOW_ANTS,
	PT_CLIP_COLOUR,///< Over only
	PT_BACKDROP,   ///< Over only
	PT_MIX,        ///< Over only

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_TEXT,
	PT_COUNT = PT_ABOUT_TEXT + 1 + stoatworks::about::kButtonCount
};

/// The ids a plugin declares, in the order the host sees them. Host index i
/// is `HostOrder( effect )[ i ]`. The About block is last in both.
const std::vector< unsigned int >& HostOrder( bool effect );

/// The group each control is shown under.
const char* GroupOf( unsigned int id );

/// The display name of a control.
const char* NameOf( unsigned int id );

/// What Reseed lays down. The Over adds Clip, LAST.
enum class Pattern
{
	Empty = 0,///< every cell colour 0: Langton's own start
	Soup,     ///< random colours at Density
	Clip,     ///< Over only: the clip's brightness, in the rule's colours
	Count
};
int PatternCount( bool effect );
const char* PatternName( Pattern pattern );

enum class Palette
{
	Phosphor = 0,
	Heat,
	Ice,
	Mono,
	Spectrum,
	Count
};
const char* PaletteName( Palette palette );

constexpr int kMaxRows = 1080;
constexpr int kMinRows = 16;
/// A host frame runs at most this many rounds; the clock drops the rest.
constexpr long long kMaxRoundsPerFrame = 100000;

//---------------------------------------------------------------------------
// The mappings.
//---------------------------------------------------------------------------

/// Rounds per second: 0 at the bottom (paused), then 1 to 1,000,000,
/// geometrically.
double SpeedFromParam( float v );
float ParamFromSpeed( double roundsPerSecond );
/// The soup's fill, 0..1.
double DensityFromParam( float v );
/// Rounds per audio onset: 0 at the bottom, then 10 to 100,000, geometrically.
long long BurstFromParam( float v );
/// Rows of the grid, from kMaxRows (v = 0) to kMinRows (v = 1), geometrically.
int RowsFromParam( float v );
float ParamFromRows( int rows );
/// The dark gap round each lit cell, as a fraction of the cell: 0 to 0.5.
double GapFromParam( float v );
/// The trail's time constant in ant steps: 0 (none) at the bottom, then 10 to
/// 1,000,000, geometrically.
double TrailFromParam( float v );
float ParamFromTrail( double steps );

/// A probability as the integer threshold a 32-bit hash is compared with.
uint32_t ThresholdU32( double probability );

struct Grid
{
	int cols = 0, rows = 0;
	bool operator==( const Grid& other ) const
	{
		return cols == other.cols && rows == other.rows;
	}
	bool operator!=( const Grid& other ) const
	{
		return !( *this == other );
	}
};
/// The grid for a raster: `rows` rows, and the columns that keep cells as
/// square as a whole number of them allows.
Grid GridFor( int rows, int width, int height );

int OptionIndex( float value, int count );

} // namespace langton
