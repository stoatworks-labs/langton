#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
    The walk. No GL: `lntest --offline` runs it in CI, and the plugin runs
    exactly this on the CPU and uploads what it touched.

    An ant stands on a cell of colour c, facing one of four ways. A step:
    turn by the rule's letter for c (R: 90 degrees clockwise, L: anticlockwise),
    advance the cell to colour c + 1 (mod the rule's length), move forward one
    cell. Langton's ant is RL: on colour 0 turn right, on colour 1 turn left.
    The grid is a torus. Several ants take turns, ant 0 first.

    **The step is a bijection**, so it has an exact inverse: move back one
    cell, step the colour back, undo the turn that colour gave. `Step( n,
    reverse )` runs either way; reversed, the ants go in the opposite order,
    so a run of n rounds forward and n back returns every cell and every ant to
    where it started, bit for bit.

    The grid's rows count UP (row 0 is the bottom, GL's order), and the four
    headings are clockwise as they appear on screen: up, right, down, left.
*/
namespace langton::ant
{
constexpr int kMaxColours = 12;
constexpr int kMaxAnts    = 8;

struct Rule
{
	const char* name; ///< what the host's Rule dropdown shows
	const char* turns;///< one letter per colour, L or R
};

/// The Rule dropdown, in order: Langton's first, then Turk and Propp's
/// many-coloured ants as Wikipedia lists them.
constexpr Rule kRules[] = {
	{ "Langton (RL)", "RL" },
	{ "LLRR", "LLRR" },
	{ "RLLR", "RLLR" },
	{ "LRRRRRLLR", "LRRRRRLLR" },
	{ "LLRRRLRLRLLR", "LLRRRLRLRLLR" },
	{ "RRLLLRLLLRRR", "RRLLLRLLLRRR" },
	{ "RLR", "RLR" },
};
constexpr int kRuleCount = static_cast< int >( sizeof( kRules ) / sizeof( kRules[ 0 ] ) );

/// Headings, clockwise on screen: 0 up (+y), 1 right (+x), 2 down, 3 left.
constexpr int kDx[ 4 ] = { 0, 1, 0, -1 };
constexpr int kDy[ 4 ] = { 1, 0, -1, 0 };

struct Ant
{
	int x = 0, y = 0;
	int heading = 0;
};

/// A rule as turns: +1 for R, -1 for L, one per colour. Empty if `turns` has
/// anything but L and R, or is shorter than 2 or longer than kMaxColours.
std::vector< int > ParseTurns( const std::string& turns );

class World
{
public:
	/// An empty grid (every cell colour 0, never visited), no ants.
	void Reset( int cols, int rows );

	/// The rule the ants obey. A grid painted under a longer rule keeps its
	/// colours; a step reads them modulo the new length. Returns false (and
	/// changes nothing) if the rule does not parse.
	bool SetRule( const std::string& turns );

	/// `count` ants at their start places: one in the middle facing up, more
	/// spread round it, each facing the next heading round. Keeps the grid.
	void PlaceAnts( int count );

	/// Exactly these ants, for the harness.
	void SetAnts( const std::vector< Ant >& ants );

	/// A soup: each cell takes a random colour from 1 to n - 1 with
	/// probability `densityU` / 2^32, by the integer hash of its place and
	/// `salt`, else 0. Keeps the ants.
	void Soup( uint32_t densityU, uint32_t salt );

	/// The grid from outside (the Over's clip), colour per cell, row 0 at the
	/// bottom. Values are taken modulo the rule's length.
	void Load( const std::vector< uint8_t >& colours );

	/// Run `rounds` rounds (every ant one step each), or back if `reverse`.
	void Step( long long rounds, bool reverse );

	/// A new size: the overlap kept, centred, cell for cell; ants keep their
	/// place relative to the middle and wrap onto the new torus.
	void Regrid( int cols, int rows );

	int Cols() const
	{
		return cols;
	}
	int Rows() const
	{
		return rows;
	}
	int Colours() const
	{
		return static_cast< int >( turns.size() );
	}
	const std::vector< uint8_t >& Grid() const
	{
		return colour;
	}
	/// The activity clock at each cell's last visit (forward or back).
	const std::vector< uint32_t >& Visits() const
	{
		return visit;
	}
	/// 1 where a cell has ever been stepped on.
	const std::vector< uint8_t >& Visited() const
	{
		return visited;
	}
	const std::vector< Ant >& Ants() const
	{
		return ants;
	}
	/// Net rounds: + forward, - back.
	long long Rounds() const
	{
		return rounds;
	}
	/// Every ant step taken, either way: what the trail is timed by.
	uint64_t Clock() const
	{
		return clock;
	}

	/// The rows touched since the last call (inclusive), or false if none.
	bool TakeDirtyRows( int& first, int& last );
	void MarkAllDirty();

	//-------------------------------------------------------------------
	// For the harness's negative controls.
	//-------------------------------------------------------------------
	/// The undo turns the WRONG way (--reverse must fail).
	void SetUnturnSignForTest( int sign )
	{
		unturnSign = sign;
	}
	/// The torus wraps one cell short on the left and bottom (--engine must fail).
	void SetWrapSkewForTest( int skew )
	{
		wrapSkew = skew;
	}

private:
	void forward( Ant& a );
	void backward( Ant& a );
	void wrap( int& x, int& y ) const;
	void dirty( int y );

	int cols = 0, rows = 0;
	std::vector< int > turns = { 1, -1 };
	std::vector< uint8_t > colour, visited;
	std::vector< uint32_t > visit;
	std::vector< Ant > ants;
	long long rounds = 0;
	uint64_t clock   = 0;
	int dirtyFirst = 1 << 30, dirtyLast = -1;
	int unturnSign = 1;
	int wrapSkew   = 0;
};

} // namespace langton::ant
