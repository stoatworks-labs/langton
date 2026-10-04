#include "Ant.h"

#include "Hash.h"

#include <algorithm>
#include <cmath>

namespace langton::ant
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

int positiveMod( int a, int b )
{
	const int m = a % b;
	return m < 0 ? m + b : m;
}
} // namespace

std::vector< int > ParseTurns( const std::string& text )
{
	std::vector< int > out;
	for( char c : text )
	{
		if( c == 'R' || c == 'r' )
			out.push_back( 1 );
		else if( c == 'L' || c == 'l' )
			out.push_back( -1 );
		else
			return {};
	}
	if( out.size() < 2 || out.size() > static_cast< size_t >( kMaxColours ) )
		return {};
	return out;
}

void World::Reset( int newCols, int newRows )
{
	cols = std::max( newCols, 1 );
	rows = std::max( newRows, 1 );
	const size_t n = static_cast< size_t >( cols ) * rows;
	colour.assign( n, 0 );
	visited.assign( n, 0 );
	visit.assign( n, 0 );
	rounds = 0;
	MarkAllDirty();
}

bool World::SetRule( const std::string& text )
{
	std::vector< int > parsed = ParseTurns( text );
	if( parsed.empty() )
		return false;
	turns = parsed;
	return true;
}

void World::PlaceAnts( int count )
{
	count = std::clamp( count, 1, kMaxAnts );
	ants.clear();
	const int radius = std::max( 2, std::min( cols, rows ) / 6 );
	for( int i = 0; i < count; ++i )
	{
		Ant a;
		a.x       = cols / 2;
		a.y       = rows / 2;
		a.heading = i % 4;
		if( i > 0 )
		{
			//The rest round the middle, evenly, the first of them to the right.
			const double angle = 2.0 * kPi * ( i - 1 ) / ( count - 1 );
			a.x += static_cast< int >( std::lround( radius * std::cos( angle ) ) );
			a.y += static_cast< int >( std::lround( radius * std::sin( angle ) ) );
		}
		a.x = positiveMod( a.x, cols );
		a.y = positiveMod( a.y, rows );
		ants.push_back( a );
	}
}

void World::SetAnts( const std::vector< Ant >& list )
{
	ants = list;
}

void World::Soup( uint32_t densityU, uint32_t salt )
{
	const int n = static_cast< int >( turns.size() );
	for( int y = 0; y < rows; ++y )
		for( int x = 0; x < cols; ++x )
		{
			const uint32_t h = CellHash( x, y, salt );
			colour[ static_cast< size_t >( y ) * cols + x ] =
				h < densityU ? static_cast< uint8_t >( 1 + Pcg( h ) % static_cast< uint32_t >( n - 1 ) ) : 0;
		}
	MarkAllDirty();
}

void World::Load( const std::vector< uint8_t >& colours )
{
	const int n = static_cast< int >( turns.size() );
	const size_t count = std::min( colours.size(), colour.size() );
	for( size_t i = 0; i < count; ++i )
		colour[ i ] = static_cast< uint8_t >( colours[ i ] % n );
	MarkAllDirty();
}

void World::wrap( int& x, int& y ) const
{
	//At most one cell outside: one add or subtract per axis.
	if( x < 0 )
		x += cols - wrapSkew;
	else if( x >= cols )
		x -= cols;
	if( y < 0 )
		y += rows - wrapSkew;
	else if( y >= rows )
		y -= rows;
}

void World::dirty( int y )
{
	dirtyFirst = std::min( dirtyFirst, y );
	dirtyLast  = std::max( dirtyLast, y );
}

void World::forward( Ant& a )
{
	const int n        = static_cast< int >( turns.size() );
	const size_t i     = static_cast< size_t >( a.y ) * cols + a.x;
	const int c        = colour[ i ] % n;
	a.heading          = ( a.heading + turns[ static_cast< size_t >( c ) ] + 4 ) & 3;
	colour[ i ]        = static_cast< uint8_t >( ( c + 1 ) % n );
	visited[ i ]       = 1;
	visit[ i ]         = static_cast< uint32_t >( clock );
	dirty( a.y );
	a.x += kDx[ a.heading ];
	a.y += kDy[ a.heading ];
	wrap( a.x, a.y );
	++clock;
}

void World::backward( Ant& a )
{
	const int n = static_cast< int >( turns.size() );
	a.x -= kDx[ a.heading ];
	a.y -= kDy[ a.heading ];
	wrap( a.x, a.y );
	const size_t i = static_cast< size_t >( a.y ) * cols + a.x;
	const int c    = ( colour[ i ] % n + n - 1 ) % n;
	colour[ i ]    = static_cast< uint8_t >( c );
	visited[ i ]   = 1;
	visit[ i ]     = static_cast< uint32_t >( clock );
	dirty( a.y );
	a.heading = ( a.heading - unturnSign * turns[ static_cast< size_t >( c ) ] + 4 ) & 3;
	++clock;
}

void World::Step( long long count, bool reverse )
{
	if( ants.empty() || cols <= 0 || rows <= 0 )
		return;
	for( long long r = 0; r < count; ++r )
	{
		if( !reverse )
		{
			for( Ant& a : ants )
				forward( a );
			++rounds;
		}
		else
		{
			for( auto it = ants.rbegin(); it != ants.rend(); ++it )
				backward( *it );
			--rounds;
		}
	}
}

void World::Regrid( int newCols, int newRows )
{
	newCols = std::max( newCols, 1 );
	newRows = std::max( newRows, 1 );
	if( newCols == cols && newRows == rows )
		return;
	//Truncated toward zero, not floored: the way back is then exactly the
	//way there negated, so 16:9 -> 4:3 -> 16:9 puts every cell back.
	const int ox = ( cols - newCols ) / 2, oy = ( rows - newRows ) / 2;
	const size_t n = static_cast< size_t >( newCols ) * newRows;
	std::vector< uint8_t > c2( n, 0 ), v2( n, 0 );
	std::vector< uint32_t > t2( n, 0 );
	for( int y = 0; y < newRows; ++y )
		for( int x = 0; x < newCols; ++x )
		{
			const int sx = x + ox, sy = y + oy;
			if( sx < 0 || sy < 0 || sx >= cols || sy >= rows )
				continue;
			const size_t from = static_cast< size_t >( sy ) * cols + sx, to = static_cast< size_t >( y ) * newCols + x;
			c2[ to ] = colour[ from ];
			v2[ to ] = visited[ from ];
			t2[ to ] = visit[ from ];
		}
	colour.swap( c2 );
	visited.swap( v2 );
	visit.swap( t2 );
	for( Ant& a : ants )
	{
		a.x = positiveMod( a.x - ox, newCols );
		a.y = positiveMod( a.y - oy, newRows );
	}
	cols = newCols;
	rows = newRows;
	MarkAllDirty();
}

bool World::TakeDirtyRows( int& first, int& last )
{
	if( dirtyLast < dirtyFirst )
		return false;
	first      = std::max( dirtyFirst, 0 );
	last       = std::min( dirtyLast, rows - 1 );
	dirtyFirst = 1 << 30;
	dirtyLast  = -1;
	return last >= first;
}

void World::MarkAllDirty()
{
	dirtyFirst = 0;
	dirtyLast  = rows - 1;
}

} // namespace langton::ant
