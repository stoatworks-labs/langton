// The C++ half of the browser demo's cross-check: the plugin's OWN walk,
// hash, clock and conversions (source/Ant.cpp, Controls.cpp, Hash.h,
// Clock.h), driven through a fixed scenario and printed. run.mjs drives the
// JavaScript port in demo/plugin.js through the same scenario and prints the
// same lines; crosscheck.sh diffs the two. Any difference is the port being
// wrong (or this scenario and run.mjs's having drifted apart -- they are
// written to be read side by side).
//
// Floats and doubles are printed as their bits, so "the same" means
// bit-identical, not "close".
//
// Built against nothing but the plugin's GL-free sources: no FFGL, no GL.

#include "Ant.h"
#include "Clock.h"
#include "Controls.h"
#include "Hash.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace langton;

namespace
{
std::string h32( uint32_t v )
{
	char s[ 16 ];
	std::snprintf( s, sizeof s, "%08x", v );
	return s;
}
std::string f32( float v )
{
	uint32_t b;
	std::memcpy( &b, &v, 4 );
	return h32( b );
}
std::string f64( double v )
{
	uint64_t b;
	std::memcpy( &b, &v, 8 );
	char s[ 24 ];
	std::snprintf( s, sizeof s, "%08x%08x", static_cast< uint32_t >( b >> 32 ), static_cast< uint32_t >( b ) );
	return s;
}

/// Langton.cpp's LangtonPlugin::seedSalt is a private member reading params[]
/// and seedSerial; this is its expression, copied verbatim, over those two.
uint32_t seedSalt( float seedParam, uint32_t seedSerial )
{
	const uint32_t seedValue = static_cast< uint32_t >( std::clamp( std::lround( seedParam ), 0L, 9999L ) );
	return Pcg( seedValue * 2654435761u ^ Pcg( seedSerial + 0x9e3779b9u ) );
}

std::string ants( const ant::World& w )
{
	std::string s;
	for( const ant::Ant& a : w.Ants() )
		s += std::to_string( a.x ) + "," + std::to_string( a.y ) + "," + std::to_string( a.heading ) + ";";
	return s.empty() ? "-" : s;
}

void state( const char* label, ant::World& w )
{
	uint32_t hc = 2166136261u, hv = 2166136261u, ht = 2166136261u;
	long long nonzero = 0, sum = 0;
	for( uint8_t c : w.Grid() )
	{
		hc ^= c;
		hc *= 16777619u;
		nonzero += c != 0;
		sum += c;
	}
	for( uint8_t v : w.Visited() )
	{
		hv ^= v;
		hv *= 16777619u;
	}
	for( uint32_t t : w.Visits() )
	{
		ht ^= t;
		ht *= 16777619u;
	}
	int first = 0, last = 0;
	const bool dirty = w.TakeDirtyRows( first, last );
	std::printf( "%s %dx%d n=%d rounds=%lld clock=%llu c=%s v=%s t=%s nz=%lld sum=%lld ants=%s dirty=%s\n", label,
	             w.Cols(), w.Rows(), w.Colours(), w.Rounds(), static_cast< unsigned long long >( w.Clock() ),
	             h32( hc ).c_str(), h32( hv ).c_str(), h32( ht ).c_str(), nonzero, sum, ants( w ).c_str(),
	             dirty ? ( std::to_string( first ) + ".." + std::to_string( last ) ).c_str() : "none" );
}

void walk( ant::World& w, const char* label, std::initializer_list< int > chunks, bool reverse )
{
	int i = 0;
	for( int n : chunks )
	{
		w.Step( n, reverse );
		const std::string l = std::string( label ) + ( reverse ? " back" : " fwd" ) + std::to_string( i++ );
		state( l.c_str(), w );
	}
}
} // namespace

int main()
{
	//-------------------------------------------------------------------
	// ParseTurns.
	//-------------------------------------------------------------------
	for( const char* text : { "RL", "rl", "LLRR", "R", "", "LRX", "LLLLLLLLLLLL", "LLLLLLLLLLLLL", "RlRl", " RL" } )
	{
		std::string s;
		for( int t : ant::ParseTurns( text ) )
			s += t > 0 ? "+" : "-";
		std::printf( "parse '%s' %s\n", text, s.empty() ? "empty" : s.c_str() );
	}

	//-------------------------------------------------------------------
	// Controls: every slider position the page can send (0.001 steps, as
	// floats), and the constructors' defaults.
	//-------------------------------------------------------------------
	for( int k = -10; k <= 1010; ++k )
	{
		const float v = static_cast< float >( k / 1000.0 );
		std::printf( "ctl %d %s speed=%s rows=%d trail=%s gap=%s density=%s thr=%s\n", k, f32( v ).c_str(),
		             f64( SpeedFromParam( v ) ).c_str(), RowsFromParam( v ), f64( TrailFromParam( v ) ).c_str(),
		             f64( GapFromParam( v ) ).c_str(), f64( DensityFromParam( v ) ).c_str(),
		             h32( ThresholdU32( DensityFromParam( v ) ) ).c_str() );
	}
	for( double s : { 600.0, 3000.0, 1.0, 1.0e6, 0.5, 2.0e6, 1234.5, 0.0, -3.0 } )
		std::printf( "paramFromSpeed %s %s back=%s\n", f64( s ).c_str(), f32( ParamFromSpeed( s ) ).c_str(),
		             f64( SpeedFromParam( ParamFromSpeed( s ) ) ).c_str() );
	for( int r = 1; r <= 1100; ++r )
		std::printf( "paramFromRows %d %s back=%d\n", r, f32( ParamFromRows( r ) ).c_str(), RowsFromParam( ParamFromRows( r ) ) );
	for( double t : { 3000.0, 10.0, 1.0e6, 5.0, 777.0, 0.0 } )
		std::printf( "paramFromTrail %s %s back=%s\n", f64( t ).c_str(), f32( ParamFromTrail( t ) ).c_str(),
		             f64( TrailFromParam( ParamFromTrail( t ) ) ).c_str() );
	for( double p : { 0.0, 1e-12, 0.2, 0.5, 1.0, 0.99999999999, std::ldexp( 1.0, -33 ), 1.0 - std::ldexp( 1.0, -33 ), -1.0,
	                  static_cast< double >( 0.2f ), static_cast< double >( 0.35f ), 2.0 } )
		std::printf( "threshold %s %s\n", f64( p ).c_str(), h32( ThresholdU32( p ) ).c_str() );
	const int rasters[][ 2 ] = { { 1280, 720 }, { 1920, 1080 }, { 640, 360 }, { 1024, 768 }, { 720, 1280 },
		                         { 333, 777 }, { 1, 1 }, { 3840, 2160 }, { 1366, 768 }, { 100, 0 } };
	for( int rows : { 1, 16, 17, 99, 120, 121, 135, 540, 1080 } )
		for( const auto& wh : rasters )
		{
			const Grid g = GridFor( rows, wh[ 0 ], wh[ 1 ] );
			std::printf( "grid %d %dx%d -> %dx%d\n", rows, wh[ 0 ], wh[ 1 ], g.cols, g.rows );
		}
	//std::lround, which the port writes by hand (Math.round rounds -2.5 to -2).
	for( double x : { -2.5, -1.5, -0.5, -0.49999999999999994, 0.49999999999999994, 0.5, 1.5, 2.5, -3.7, 3.7, -1e9 - 0.5 } )
		std::printf( "lround %s %ld\n", f64( x ).c_str(), std::lround( x ) );
	for( float v : { -1.5f, -0.5f, -0.49f, 0.0f, 0.49f, 0.5f, 0.51f, 1.5f, 2.5f, 6.5f, 7.0f, 100.0f } )
		for( int count : { 2, 3, 5, 7, 8 } )
			std::printf( "option %s %d -> %d\n", f32( v ).c_str(), count, OptionIndex( v, count ) );

	//-------------------------------------------------------------------
	// The hash, and the seed's salt.
	//-------------------------------------------------------------------
	for( uint32_t i = 0; i < 256; ++i )
		std::printf( "pcg %u %s\n", i, h32( Pcg( i ) ).c_str() );
	{
		uint32_t x = 0x12345678u;
		for( int i = 0; i < 2000; ++i )
		{
			x = Pcg( x );
			if( i % 50 == 0 )
				std::printf( "pcgchain %d %s\n", i, h32( x ).c_str() );
		}
	}
	for( uint32_t v : { 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu } )
		std::printf( "pcg %s %s\n", h32( v ).c_str(), h32( Pcg( v ) ).c_str() );
	for( int x : { 0, 1, 1919 } )
		for( int y : { 0, 1, 1079 } )
			for( uint32_t salt : { 0u, 1u, 0xdeadbeefu } )
				std::printf( "cellhash %d %d %s %s\n", x, y, h32( salt ).c_str(), h32( CellHash( x, y, salt ) ).c_str() );
	for( float seed : { 0.0f, 1.0f, 2.0f, 99.0f, 9999.0f, 10000.0f, -1.0f, 1.5f, 2.5f, 0.49f } )
		for( uint32_t serial : { 0u, 1u, 2u, 0xffffffffu } )
			std::printf( "salt %s %s %s\n", f32( seed ).c_str(), h32( serial ).c_str(), h32( seedSalt( seed, serial ) ).c_str() );

	//-------------------------------------------------------------------
	// The step clock: 3000 frames at each rate, every 250th printed, and a
	// digest of every frame's count.
	//-------------------------------------------------------------------
	{
		struct Case
		{
			double speed, fps;
			bool jitter;
		};
		const double speeds[] = { SpeedFromParam( ParamFromSpeed( 600.0 ) ), SpeedFromParam( ParamFromSpeed( 3000.0 ) ),
			                      1.0, 600.0, 1.0e6, SpeedFromParam( 0.3f ), 7.3 };
		int caseIndex = 0;
		for( double speed : speeds )
			for( double fps : { 60.0, 23.976, 5.0, 144.0 } )
				for( bool jitter : { false, true } )
				{
					StepClock clock;
					uint32_t digest = 2166136261u;
					long long total = 0;
					for( int i = 0; i < 3000; ++i )
					{
						double dt = 1.0 / fps;
						if( jitter )
							dt = dt * ( 0.5 + Pcg( static_cast< uint32_t >( i ) ) / 4294967296.0 );
						const long long whole = clock.Advance( speed, dt, kMaxRoundsPerFrame );
						total += whole;
						digest ^= static_cast< uint32_t >( whole );
						digest *= 16777619u;
						if( i % 250 == 0 )
							std::printf( "clock %d %d %lld %s\n", caseIndex, i, whole, f64( clock.Fraction() ).c_str() );
					}
					std::printf( "clockcase %d speed=%s fps=%s jitter=%d total=%lld digest=%s owed=%s\n", caseIndex,
					             f64( speed ).c_str(), f64( fps ).c_str(), jitter ? 1 : 0, total, h32( digest ).c_str(),
					             f64( clock.Fraction() ).c_str() );
					++caseIndex;
				}
		StepClock edge;
		std::printf( "clockedge %lld %lld %lld", edge.Advance( 0.0, 0.1, 10 ), edge.Advance( 5.0, 0.0, 10 ),
		             edge.Advance( 5.0, -1.0, 10 ) );
		std::printf( " %lld %s", edge.Advance( 1.0e6, 0.2, kMaxRoundsPerFrame ), f64( edge.Fraction() ).c_str() );
		std::printf( " %lld %s\n", edge.Advance( 1.0e6, 0.2, kMaxRoundsPerFrame ), f64( edge.Fraction() ).c_str() );
	}

	//-------------------------------------------------------------------
	// The walk: every rule x ant counts x grids x grounds, out and back.
	//-------------------------------------------------------------------
	const int walkSizes[][ 2 ] = { { 157, 93 }, { 16, 9 }, { 213, 120 } };
	for( int r = 0; r < ant::kRuleCount; ++r )
		for( int count : { 1, 2, 3, 4, 5, 8 } )
			for( const auto& size : walkSizes )
				for( int ground = 0; ground < 3; ++ground )
				{
					ant::World w;
					w.Reset( size[ 0 ], size[ 1 ] );
					w.SetRule( ant::kRules[ r ].turns );
					w.PlaceAnts( count );
					if( ground > 0 )
						w.Soup( ThresholdU32( ground == 1 ? 0.2 : 0.9 ), seedSalt( static_cast< float >( r + 1 ), static_cast< uint32_t >( count ) ) );
					const std::string label = "walk r" + std::to_string( r ) + " a" + std::to_string( count ) + " "
					                          + std::to_string( size[ 0 ] ) + "x" + std::to_string( size[ 1 ] )
					                          + " g" + std::to_string( ground );
					state( ( label + " seed" ).c_str(), w );
					walk( w, label.c_str(), { 1, 9, 90, 900, 2000 }, false );
					walk( w, label.c_str(), { 2000, 900, 90, 9, 1 }, true );
				}

	// Langton's own: 12,000 steps from an empty 512 x 512 grid, into the highway and back.
	{
		ant::World w;
		w.Reset( 512, 512 );
		w.SetRule( "RL" );
		w.PlaceAnts( 1 );
		walk( w, "highway", { 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 977, 1023, 1000, 1000 }, false );
		walk( w, "highway", { 6000, 6000 }, true );
	}

	// Ants that start on the edges, facing off them: the torus both ways.
	for( int r = 0; r < ant::kRuleCount; ++r )
	{
		ant::World w;
		w.Reset( 31, 17 );
		w.SetRule( ant::kRules[ r ].turns );
		w.SetAnts( { { 0, 0, 3 }, { 30, 0, 1 }, { 0, 16, 0 }, { 30, 16, 2 }, { 15, 0, 2 }, { 0, 8, 3 } } );
		w.Soup( ThresholdU32( 0.5 ), 0xc0ffeeu + static_cast< uint32_t >( r ) );
		const std::string label = "edges r" + std::to_string( r );
		state( ( label + " seed" ).c_str(), w );
		walk( w, label.c_str(), { 1, 1, 1, 7, 490, 2000 }, false );
		walk( w, label.c_str(), { 2000, 490, 7, 1, 1, 1 }, true );
	}

	// A Rule change mid-walk: the field carries on, read modulo the new length.
	{
		ant::World w;
		w.Reset( 101, 61 );
		w.SetRule( ant::kRules[ 4 ].turns );
		w.PlaceAnts( 3 );
		w.Soup( ThresholdU32( 0.6 ), 77u );
		state( "rules seed", w );
		walk( w, "rules LLRRRLRLRLLR", { 1500 }, false );
		std::printf( "setrule LLRR %d\n", w.SetRule( "LLRR" ) ? 1 : 0 );
		walk( w, "rules LLRR", { 1500 }, false );
		std::printf( "setrule RLR %d\n", w.SetRule( "RLR" ) ? 1 : 0 );
		walk( w, "rules RLR", { 700 }, true );
		std::printf( "setrule RRLLLRLLLRRR %d\n", w.SetRule( "RRLLLRLLLRRR" ) ? 1 : 0 );
		walk( w, "rules RRLLLRLLLRRR", { 1000 }, false );
		std::printf( "setrule bogus %d\n", w.SetRule( "bogus" ) ? 1 : 0 );
		walk( w, "rules still RRLLLRLLLRRR", { 10 }, false );
	}

	// Re-grids: 16:9 -> 4:3 -> 16:9 keeps the middle; then smaller, larger, a no-op, and 1 x 1.
	{
		ant::World w;
		w.Reset( 213, 120 );
		w.SetRule( "RL" );
		w.PlaceAnts( 4 );
		w.Soup( ThresholdU32( 0.3 ), 5u );
		walk( w, "regrid", { 2000 }, false );
		w.Regrid( 160, 120 );
		state( "regrid 160x120", w );
		walk( w, "regrid a", { 500 }, false );
		w.Regrid( 213, 120 );
		state( "regrid 213x120", w );
		w.Regrid( 100, 57 );
		state( "regrid 100x57", w );
		walk( w, "regrid b", { 1000 }, true );
		w.Regrid( 300, 200 );
		state( "regrid 300x200", w );
		w.Regrid( 300, 200 );
		state( "regrid same", w );
		w.Regrid( 0, -5 );
		state( "regrid 1x1", w );
		walk( w, "regrid c", { 10 }, false );
	}

	// Load: the Over's ground, taken modulo the rule's length; short and long vectors.
	{
		ant::World w;
		w.Reset( 37, 23 );
		w.SetRule( ant::kRules[ 3 ].turns );
		w.PlaceAnts( 2 );
		for( size_t length : { static_cast< size_t >( 37 * 23 ), static_cast< size_t >( 100 ), static_cast< size_t >( 37 * 23 + 50 ) } )
		{
			std::vector< uint8_t > colours( length );
			for( size_t i = 0; i < length; ++i )
				colours[ i ] = static_cast< uint8_t >( ( i * 37 + length ) & 255 );
			w.Load( colours );
			const std::string label = "load " + std::to_string( length );
			state( label.c_str(), w );
			walk( w, label.c_str(), { 1000 }, false );
		}
	}

	// Steps that do nothing.
	{
		ant::World w;
		w.Step( 10, false );
		state( "fresh", w );
		w.Reset( 9, 7 );
		w.Step( 10, false );
		state( "noants", w );
		w.PlaceAnts( 0 );
		w.Step( 0, false );
		state( "zero", w );
		w.PlaceAnts( 99 );
		state( "clamped", w );
	}

	// Where the ants start, for every grid shape up to 1080 rows.
	{
		std::vector< std::pair< int, int > > sizes;
		for( int n = 1; n <= 40; ++n )
			sizes.push_back( { n, n } );
		for( int n = 42; n <= 1080; n += 6 )
		{
			sizes.push_back( { n, n } );
			sizes.push_back( { n + 1, n + 5 } );
		}
		for( auto wh : std::initializer_list< std::pair< int, int > > { { 1920, 1080 }, { 1080, 1920 }, { 7, 3 }, { 3, 7 }, { 213, 120 }, { 157, 93 } } )
			sizes.push_back( wh );
		for( auto [ cols, rows ] : sizes )
		{
			ant::World w;
			w.Reset( cols, rows );
			for( int count = 1; count <= ant::kMaxAnts; ++count )
			{
				w.PlaceAnts( count );
				std::printf( "place %dx%d %d %s\n", cols, rows, count, ants( w ).c_str() );
			}
		}
	}
	return 0;
}
