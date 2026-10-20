/**
    lntest -- render Langton offline, and measure what the ants are doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context. The walk is the
    plugin's own engine (ant::World); the checks read it, and read back the
    RG32UI state texture the plugin uploads, and the picture.

        lntest --out /tmp/life.png       the source, the defaults
        lntest --over --out /tmp/o.png   the Over effect on the harness's card
        lntest --list                    every parameter and its default
        lntest --pipe [--frames N]       the source's frames, raw RGBA on stdout
        lntest --over --pipe             raw frames in, raw frames out
        lntest --film N                  N frames, raw RGBA on stdout
        lntest --offline                 the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value` lines,
    held before the first key and after the last. A STANDARD (0..1) control is
    linear between its keys; an option, a boolean, an event or an integer
    STEPS: it holds each key's value until the next key's frame. So a button
    press is three keys (0, 1, 0) and a dropdown never passes through the
    options between two keys.

    LNTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.

    The claims, one flag each -- see README "Building and testing". The
    literature (Langton 1986; Turk and Propp's many-coloured ants) is what
    Wikipedia's article states; `--literature` measures the plugin's engine
    against it, `--engine` holds the engine to the harness's own stepper, and
    the GL checks take the same walk through the real plugin.
*/

#include "Ant.h"
#include "Clock.h"
#include "Controls.h"
#include "Hash.h"
#include "Langton.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace langton;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;
using Cells  = std::vector< uint32_t >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written top
/// row first, which is the only place anything here flips. Premultiplied
/// output is written as it is: a viewer shows the source over black.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				//Alpha written as 1: a premultiplied source over black is what
				//Resolume's Add shows, and a viewer that honours alpha would
				//otherwise show the dead cells as the page behind.
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context. LNTEST_RENDERER=software asks for Apple's software renderer
// by id (plotter's recipe, via radar): what a GPU-less runner falls back to.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "LNTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "lntest: LNTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_INTEGER: return "integer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch, a press or a count: cues
/// STEP between keys for these, and only a standard control ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( parts.back().c_str(), nullptr );
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

/// The value at `frame`: held before the first key and after the last; between
/// two keys linear if `ramp`, otherwise the earlier key's value until the
/// later key's frame.
float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// Pictures.
//---------------------------------------------------------------------------
double hash01( uint32_t a, uint32_t b = 0 )
{
	return Pcg( a * 2654435761u ^ Pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// The Over effect's card: a night street -- a dark ground, a lit skyline
/// along the top, bright windows, a ring, a coloured sign (saturated: luma
/// would miss it, the brightest channel does not). Rows bottom first.
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double s = std::min( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 - 0.5 * width ) / s, v = ( y + 0.5 - 0.5 * height ) / s;
			double r = 0.04 + 0.03 * ( 0.5 + v ), g = r, b = r * 1.3;
			if( v > 0.22 && v < 0.42 )
			{
				const int block = static_cast< int >( std::floor( ( u + 2.0 ) * 14.0 ) );
				const int row   = static_cast< int >( std::floor( ( v - 0.22 ) * 40.0 ) );
				if( hash01( static_cast< uint32_t >( block * 31 + row ), 3 ) > 0.45 )
					r = g = b = 0.6 + 0.4 * hash01( static_cast< uint32_t >( block ), 4 );
			}
			const double rr = std::sqrt( ( u + 0.4 ) * ( u + 0.4 ) + ( v + 0.15 ) * ( v + 0.15 ) );
			if( std::fabs( rr - 0.14 ) < 0.02 )
				r = g = b = 0.95;
			if( std::fabs( u - 0.35 ) < 0.18 && std::fabs( v + 0.2 ) < 0.07 )
			{
				r = 0.05;
				g = 0.2;
				b = 0.95;
			}
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( r );
			o[ 1 ]   = static_cast< float >( g );
			o[ 2 ]   = static_cast< float >( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// Audio, written into the Audio buffer the way the host writes it.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,
	Pulses///< a bass-heavy spectrum with a hit every half second
};

void feedAudio( LangtonPlugin& plugin, double seconds, AudioFeed feed )
{
	const int host = plugin.HostIndexOf( PT_AUDIO );
	if( host < 0 )
		return;
	const double beat  = std::fmod( std::max( seconds, 0.0 ), 0.5 );
	const float strike = feed == AudioFeed::Pulses ? static_cast< float >( 0.15 + 1.5 * std::exp( -beat / 0.06 ) ) : 0.0f;
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( audio::kBins - 1 );
		const float shape  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( static_cast< unsigned int >( host ), static_cast< unsigned int >( bin ), shape * strike );
	}
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock.
//---------------------------------------------------------------------------
struct Rig
{
	LangtonPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0, readFBO = 0;
	int frame          = 0;
	double fps         = 60.0;
	double clockOffset = 0.0;
	/// The host's clock unit: 1 sends seconds, 1000 milliseconds.
	double hostUnit = 1.0;
	AudioFeed feed  = AudioFeed::Silence;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	explicit Rig( bool effect = false ) : plugin( effect )
	{
	}

	~Rig()
	{
		plugin.DeInitGL();
		release();
		if( readFBO )
			glDeleteFramebuffers( 1, &readFBO );
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO = outputFBO;
		if( plugin.IsEffect() )
		{
			const Floats card = picture ? *picture : buildCard( width, height );
			sourceTexture     = makeTexture( width, height, card.data() );
			inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
			inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
			inputStruct.Handle                              = sourceTexture;
			inputs[ 0 ]                                     = &inputStruct;
			process.numInputTextures                        = 1;
			process.inputTextures                           = inputs;
		}
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr, bool fixClock = true )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/langton for which shader\n" );
			return false;
		}
		if( fixClock )
			plugin.SetClockScaleForTest( 1.0 );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL (Arena
	/// calls neither InitGL nor FF_RESIZE on a running clip).
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetById( id, value );
	}

	double TimeOf( int f ) const
	{
		return clockOffset + static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			const double seconds = TimeOf( frame );
			plugin.SetTime( seconds * hostUnit );
			feedAudio( plugin, seconds, feed );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	/// Exactly `n` rounds, in one frame, the clock aside.
	bool Rounds( long long n )
	{
		plugin.RequestRoundsForTest( n );
		return Render( 1 );
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// The state texture as the GPU holds it: (R, G) per cell, cols x rows,
	/// row 0 at the bottom. R is the colour | 0x100 if visited, G the visit.
	Cells Texture()
	{
		const Grid grid = plugin.CurrentGrid();
		Cells cells( static_cast< size_t >( grid.cols ) * grid.rows * 2 );
		if( !readFBO )
			glGenFramebuffers( 1, &readFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, readFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, plugin.StateTextureID(), 0 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, grid.cols, grid.rows, GL_RG_INTEGER, GL_UNSIGNED_INT, cells.data() );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return cells;
	}

	/// The colours from the texture, one byte a cell.
	std::vector< uint8_t > TextureColours()
	{
		const Cells t = Texture();
		std::vector< uint8_t > out( t.size() / 2 );
		for( size_t i = 0; i < out.size(); ++i )
			out[ i ] = static_cast< uint8_t >( t[ i * 2 ] & 0xFFu );
		return out;
	}
};

/// A rig stepped by hand: the ants move only when asked, their clock stopped,
/// no audio behind the check's back.
void manual( Rig& rig )
{
	rig.Set( PT_SPEED, 0.0f );
	rig.Set( PT_AUDIO_BURST, 0.0f );
	rig.Set( PT_AUDIO_REVERSE, 0.0f );
	rig.Set( PT_REVERSE, 0.0f );
}

//===========================================================================
// The harness's own walk. Written from Langton's definition with direction
// VECTORS turned by hand, not the engine's heading table and its modular
// arithmetic, so `--engine` compares two readings of the definition, not one
// copied twice. It only goes forward: the engine's way back is checked by
// arriving where the walk started.
//===========================================================================
struct RefAnt
{
	int x, y, dx, dy;
};

struct Walk
{
	int cols, rows;
	std::string rule;
	std::vector< uint8_t > colour, visited;
	std::vector< uint32_t > visit;
	std::vector< RefAnt > ants;
	uint64_t clock = 0;
	long long crossings = 0;///< steps that left the grid and came in the other side

	Walk( int c, int r, const std::string& turns ) :
		cols( c ),
		rows( r ),
		rule( turns ),
		colour( static_cast< size_t >( c ) * r, 0 ),
		visited( colour.size(), 0 ),
		visit( colour.size(), 0 )
	{
	}

	/// One round: each ant in turn reads its cell, turns (R clockwise as the
	/// screen shows it, rows counting up), repaints the cell and walks on.
	void Step()
	{
		const int n = static_cast< int >( rule.size() );
		for( RefAnt& a : ants )
		{
			const size_t i = static_cast< size_t >( a.y ) * cols + a.x;
			const int c    = colour[ i ] % n;
			const int dx = a.dx, dy = a.dy;
			if( rule[ static_cast< size_t >( c ) ] == 'R' )
			{
				a.dx = dy;//up (0, 1) becomes right (1, 0)
				a.dy = -dx;
			}
			else
			{
				a.dx = -dy;//up (0, 1) becomes left (-1, 0)
				a.dy = dx;
			}
			colour[ i ]  = static_cast< uint8_t >( ( c + 1 ) % n );
			visited[ i ] = 1;
			visit[ i ]   = static_cast< uint32_t >( clock++ );
			const int nx = a.x + a.dx, ny = a.y + a.dy;
			a.x          = ( nx + cols ) % cols;
			a.y          = ( ny + rows ) % rows;
			crossings += nx != a.x || ny != a.y;
		}
	}
};

/// The engine's ant as a vector: Ant.h's convention, headings clockwise from up.
RefAnt asVector( const ant::Ant& a )
{
	static const int dx[ 4 ] = { 0, 1, 0, -1 };
	static const int dy[ 4 ] = { 1, 0, -1, 0 };
	return RefAnt { a.x, a.y, dx[ a.heading & 3 ], dy[ a.heading & 3 ] };
}

std::vector< RefAnt > asVectors( const std::vector< ant::Ant >& ants )
{
	std::vector< RefAnt > out;
	for( const ant::Ant& a : ants )
		out.push_back( asVector( a ) );
	return out;
}

bool sameAnts( const std::vector< ant::Ant >& a, const std::vector< RefAnt >& b )
{
	if( a.size() != b.size() )
		return false;
	for( size_t i = 0; i < a.size(); ++i )
	{
		const RefAnt r = asVector( a[ i ] );
		if( r.x != b[ i ].x || r.y != b[ i ].y || r.dx != b[ i ].dx || r.dy != b[ i ].dy )
			return false;
	}
	return true;
}

bool sameAnts( const std::vector< ant::Ant >& a, const std::vector< ant::Ant >& b )
{
	if( a.size() != b.size() )
		return false;
	for( size_t i = 0; i < a.size(); ++i )
		if( a[ i ].x != b[ i ].x || a[ i ].y != b[ i ].y || a[ i ].heading != b[ i ].heading )
			return false;
	return true;
}

template< typename T >
int countDiff( const std::vector< T >& a, const std::vector< T >& b )
{
	if( a.size() != b.size() )
		return static_cast< int >( std::max( a.size(), b.size() ) );
	int n = 0;
	for( size_t i = 0; i < a.size(); ++i )
		n += a[ i ] != b[ i ];
	return n;
}

int countNonZero( const std::vector< uint8_t >& cells )
{
	int n = 0;
	for( uint8_t c : cells )
		n += c != 0;
	return n;
}

/// The texture the plugin should have uploaded for `walk`: colour | visited
/// bit, and the step it was last visited on.
int texelsWrong( const Cells& texture, const Walk& walk )
{
	int wrong = 0;
	for( size_t i = 0; i < walk.colour.size(); ++i )
	{
		const uint32_t r = walk.colour[ i ] | ( walk.visited[ i ] ? 0x100u : 0u );
		wrong += texture[ i * 2 ] != r || texture[ i * 2 + 1 ] != walk.visit[ i ];
	}
	return wrong;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( LangtonPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

bool applySetting( LangtonPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

/// The cue sheet bound to a plugin's parameters, or an error naming the cue.
bool bindScript( LangtonPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock.
//===========================================================================
/// `readStdin`: the Over effect's frames come in on stdin, one out per one in,
/// until a partial frame or EOF. Otherwise frames are made -- `count` of them,
/// or, with `count` 0, until the reader hangs up (so that mode only ever ends
/// with exit 1; use a count for a take that can end cleanly).
int runPipe( bool effect, int width, int height, double fps, const std::string& scriptPath, int count, bool readStdin,
             bool beat, const std::vector< std::string >& settings )
{
	Rig rig( effect );
	rig.fps = fps;
	if( !rig.Init( width, height ) )
		return 1;
	if( beat )
		rig.feed = AudioFeed::Pulses;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	//A misspelt cue that silently did nothing would film a take that looks
	//deliberate and is wrong: refuse any name that is not a parameter.
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || count <= 0 || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			if( effect )
				rig.Upload( picture );
		}

		//Through the plugin's own setter, so a cue moves what a slider would.
		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
			{
				//The source is premultiplied: written over black, alpha opaque.
				const float v = ( !effect && x % 4 == 3 ) ? 1.0f : out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ];
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
			}
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone (`| head -c 1`, ffmpeg dying). SIGPIPE is
			//ignored in main(), so this is EPIPE and not a silent 141: say so
			//and stop, rather than render on into a closed pipe.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// --bench
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	std::printf( "\n=== bench: after 120 frames of warm-up, each frame timed with glFinish both sides\n" );
	struct Case
	{
		bool effect;
		int rows, ants;
		double speed;
		const char* what;
	};
	const Case cases[] = { { false, 120, 1, 600.0, "defaults (120 rows, 1 ant)" },
		                   { true, 120, 4, 3000.0, "defaults (120 rows, 4 ants)" },
		                   { false, 1080, 8, 1.0e6, "1080 rows, 8 ants, 1e6/s" } };
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig( c.effect );
			if( !rig.Init( size.w, size.h ) )
				return 1;
			rig.Set( PT_CELL_SIZE, ParamFromRows( c.rows ) );
			rig.Set( PT_ANTS, static_cast< float >( c.ants ) );
			rig.Set( PT_SPEED, ParamFromSpeed( c.speed ) );
			if( !rig.Render( 120 ) )
				return 1;
			glFinish();
			constexpr int kTimed = 120;
			std::vector< double > times;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				if( !rig.Render( 1 ) )
					return 1;
				glFinish();
				times.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			double mean = 0.0;
			for( double t : times )
				mean += t / kTimed;
			std::sort( times.begin(), times.end() );
			std::printf( "  %-16s %-28s %-6s median %5.2f ms/frame, mean %5.2f, worst %5.2f  (median %4.1f%% of a 60 fps frame)\n",
			             c.effect ? "SW Langton Over" : "SW Langton", c.what, size.name, times[ kTimed / 2 ], mean, times.back(),
			             100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ) );
		}
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the literature or the stated law; `--negative` sets one
// field at a time to a deliberately wrong MODEL (a test hook in the plugin,
// or the harness's own expectation made wrong) and requires the check to
// FAIL.
//===========================================================================
struct Perturb
{
	bool literatureMirror = false;///< --literature: Langton's ant read mirror-image (LR)
	bool engineWrapSkew   = false;///< --engine: the torus wraps one cell short
	bool dropRow          = false;///< --highway: the upload skips the last row it touched
	bool unturn           = false;///< --reverse: the undo turns the wrong way
	bool lawPerFrame      = false;///< --clock-law: the owed fraction carried in float
	bool clockFloat       = false;///< --clock: elapsed time from a float host clock
	bool pointSample      = false;///< --coverage: the cell under the pixel's centre
	bool lookPerRound     = false;///< --look: the harness times the trail in rounds, not steps
	bool lookRawColour    = false;///< --look: a leftover colour expected as itself, not modulo the rule
	bool overMirrored     = false;///< --over-check: the clip laid under the grid mirrored
	bool primeOff         = false;///< --prime: the analyser unprimed
	bool audioTurnKept    = false;///< --prime: expect an onset's turn to outlast Audio Reverse
	bool resizeClears     = false;///< --resize: the walk cleared on a resize
	bool cuesRamp         = false;///< --cues: every control ramps between keys
};

using CheckFn = int ( * )( const Perturb& );

/// The rasters every check runs at: the one developed at, and CI's.
struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

/// The look's colours, as the palette table states them (Langton.cpp).
struct LookColours
{
	float first[ 3 ], last[ 3 ], trail[ 3 ], ant[ 3 ];
};
const LookColours kPhosphor = { { 0.80f, 1.00f, 0.80f }, { 0.05f, 0.50f, 0.15f }, { 0.20f, 0.70f, 0.30f }, { 1.00f, 0.30f, 0.15f } };

//===========================================================================
// --literature (no GL): the plugin's engine against what is published.
// Langton (1986): the ant on an empty grid wanders for about 10,000 steps,
// then builds a "highway", a 104-step cycle that repeats for ever, moving it
// diagonally. Turk and Propp: a rule made of pairs of equal letters (LL, RR)
// grows symmetrically. And the step is a bijection, so every walk runs back.
//===========================================================================
int runLiterature( const Perturb& perturb )
{
	std::printf( "\n=== literature: Langton's highway after ~10,000 steps; LL/RR-pair rules grow symmetrically; every walk runs back\n" );
	constexpr int kSide = 512, kHome = 256, kHorizon = 12000;
	{
		ant::World w;
		w.Reset( kSide, kSide );
		w.SetRule( perturb.literatureMirror ? "LR" : "RL" );
		w.SetAnts( { ant::Ant { kHome, kHome, 0 } } );
		std::vector< int > turn( kHorizon );
		std::vector< std::pair< int, int > > place( kHorizon + 1, { 0, 0 } );
		int black10k = -1, black11k = -1;
		for( int t = 0; t < kHorizon; ++t )
		{
			const int before = w.Ants()[ 0 ].heading;
			w.Step( 1, false );
			const ant::Ant& a = w.Ants()[ 0 ];
			turn[ static_cast< size_t >( t ) ]      = ( a.heading - before + 4 ) & 3;//1 right, 3 left
			place[ static_cast< size_t >( t + 1 ) ] = { a.x - kHome, a.y - kHome };
			if( t + 1 == 10000 )
				black10k = countNonZero( w.Grid() );
			if( t + 1 == 11000 )
				black11k = countNonZero( w.Grid() );
		}
		//The onset: the first step from which every turn is the turn 104
		//steps later, to the horizon.
		int onset = kHorizon - 104;
		while( onset > 0 && turn[ static_cast< size_t >( onset - 1 ) ] == turn[ static_cast< size_t >( onset - 1 + 104 ) ] )
			--onset;
		int periods = 0;
		bool drift  = true;
		for( int k = 1; onset + 104 * k <= kHorizon; ++k, ++periods )
		{
			const auto& p = place[ static_cast< size_t >( onset + 104 * k ) ];
			const auto& o = place[ static_cast< size_t >( onset ) ];
			drift         = drift && p.first - o.first == -2 * k && p.second - o.second == -2 * k;
		}
		int x0 = kSide, x1 = -kSide, y0 = kSide, y1 = -kSide;
		for( int y = 0; y < kSide; ++y )
			for( int x = 0; x < kSide; ++x )
				if( w.Visited()[ static_cast< size_t >( y ) * kSide + x ] )
				{
					x0 = std::min( x0, x - kHome );
					x1 = std::max( x1, x - kHome );
					y0 = std::min( y0, y - kHome );
					y1 = std::max( y1, y - kHome );
				}
		const auto& end = place.back();
		Check( onset == 9977 && periods >= 19 && drift,
		       fmt( "Langton's ant (RL), empty grid: the turns repeat every 104 steps from step %d on (measured 9977) to step "
		            "%d, %d whole cycles, each moving it (-2, -2) (%s)",
		            onset, kHorizon, periods, drift ? "every one" : "NOT every one" ) );
		Check( black10k == 720 && black11k == 834 && x0 == -57 && x1 == 29 && y0 == -34 && y1 == 22 && end.first == -56
		           && end.second == -32,
		       fmt( "the same walk: %d black cells at step 10,000 and %d at 11,000 (720 and 834, measured by two steppers); at "
		            "12,000 it has visited x %d..%d, y %d..%d and stands at (%d, %d)",
		            black10k, black11k, x0, x1, y0, y1, end.first, end.second ) );
	}
	//Symmetry: reflected in the line between the start's row and the one
	//below, at every return to the start facing up.
	struct Sym
	{
		const char* rule;
		bool pairs;
	};
	for( const Sym& s : { Sym { "LLRR", true }, Sym { "RLLR", true }, Sym { "RL", false } } )
	{
		ant::World w;
		w.Reset( kSide, kSide );
		w.SetRule( perturb.literatureMirror && s.pairs ? "LLRL" : s.rule );
		w.SetAnts( { ant::Ant { kHome, kHome, 0 } } );
		int returns = 0, symmetric = 0, lastReturn = 0;
		for( int t = 1; t <= 20000; ++t )
		{
			w.Step( 1, false );
			const ant::Ant& a = w.Ants()[ 0 ];
			if( a.x != kHome || a.y != kHome || a.heading != 0 )
				continue;
			++returns;
			lastReturn = t;
			bool same  = true;
			const auto& g = w.Grid();
			for( int y = 0; y < kSide && same; ++y )
			{
				const int my = 2 * kHome - 1 - y;
				for( int x = 0; x < kSide && same; ++x )
					same = g[ static_cast< size_t >( y ) * kSide + x ] == g[ static_cast< size_t >( my ) * kSide + x ];
			}
			symmetric += same;
		}
		if( s.pairs )
			Check( returns >= 3 && symmetric == returns,
			       fmt( "%s (pairs of equal letters): back at the start facing up %d times in 20,000 steps (the last at "
			            "step %d), mirror-symmetric %d of them",
			            s.rule, returns, lastReturn, symmetric ) );
		else
			Check( returns > symmetric,
			       fmt( "%s (no pairs), the contrast: back at the start facing up %d times in 20,000 steps, mirror-symmetric "
			            "only %d of them",
			            s.rule, returns, symmetric ) );
	}
	//Turk and Propp's LRRRRRLLR "fills space in a square around itself": the
	//cells it has visited are a square, almost all of it, at every size.
	{
		ant::World w;
		w.Reset( kSide, kSide );
		w.SetRule( "LRRRRRLLR" );
		w.SetAnts( { ant::Ant { kHome, kHome, 0 } } );
		bool square = true;
		std::string what;
		long long done = 0;
		for( long long at : { 20000LL, 40000LL, 80000LL, 160000LL } )
		{
			w.Step( at - done, false );
			done = at;
			int x0 = kSide, x1 = -1, y0 = kSide, y1 = -1, visited = 0;
			for( int y = 0; y < kSide; ++y )
				for( int x = 0; x < kSide; ++x )
					if( w.Visited()[ static_cast< size_t >( y ) * kSide + x ] )
					{
						x0 = std::min( x0, x );
						x1 = std::max( x1, x );
						y0 = std::min( y0, y );
						y1 = std::max( y1, y );
						++visited;
					}
			const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
			const double fill = static_cast< double >( visited ) / ( static_cast< double >( bw ) * bh );
			square = square && std::abs( bw - bh ) <= bw / 50 + 1 && fill >= 0.95;
			what += fmt( " %lld: %dx%d %.1f%%;", at, bw, bh, 100.0 * fill );
		}
		Check( square, fmt( "LRRRRRLLR fills a square: the cells it has visited, their bounding box and how much of it "
		                    "(square to 2%%, at least 95%%):%s",
		                    what.c_str() ) );
	}
	//The way back: every rule, 20,000 steps out and 20,000 back.
	{
		int failed = 0;
		std::string what;
		for( const ant::Rule& rule : ant::kRules )
		{
			ant::World w;
			w.Reset( kSide, kSide );
			w.SetRule( rule.turns );
			w.SetAnts( { ant::Ant { kHome, kHome, 0 } } );
			w.Step( 20000, false );
			const int painted = countNonZero( w.Grid() );
			w.Step( 20000, true );
			const bool home = countNonZero( w.Grid() ) == 0 && sameAnts( w.Ants(), { ant::Ant { kHome, kHome, 0 } } )
			                  && w.Rounds() == 0;
			failed += !home;
			what += fmt( " %s(%d)%s", rule.turns, painted, home ? "" : "-NOT-BACK" );
		}
		Check( failed == 0, fmt( "every rule 20,000 steps out (cells painted) and back to an empty grid and the ant home "
		                         "facing up:%s",
		                         what.c_str() ) );
	}
	//The parser takes the rule table and nothing else.
	{
		bool ok = ant::ParseTurns( "RL" ) == std::vector< int > { 1, -1 } && ant::ParseTurns( "llrr" ).size() == 4
		          && ant::ParseTurns( "RRLLLRLLLRRR" ).size() == 12;
		for( const char* bad : { "", "R", "RLX", "R L", "RRLLLRLLLRRRL" } )
			ok = ok && ant::ParseTurns( bad ).empty();
		for( const ant::Rule& rule : ant::kRules )
			ok = ok && !ant::ParseTurns( rule.turns ).empty();
		Check( ok, "the rule parser: every rule in the table, either case; nothing shorter than 2, longer than 12, or not L/R" );
	}
	return Verdict();
}

//===========================================================================
// --engine (no GL): the plugin's walk is the harness's own, cell for cell.
//===========================================================================
int runEngine( const Perturb& perturb )
{
	std::printf( "\n=== engine: the walk the plugin runs is the harness's, every rule, 1, 8 and 4 edge ants, across the edges, and back\n" );
	constexpr int kCols = 157, kRows = 93;
	int soupBad = 0;
	std::string soupWhat;
	for( int r = 0; r < ant::kRuleCount; ++r )
	{
		const std::string turns = ant::kRules[ r ].turns;
		int wrongCells = 0, wrongAnts = 0, notBack = 0;
		long long crossings = 0;
		for( int set = 0; set < 3; ++set )
		{
			ant::World w;
			w.SetWrapSkewForTest( perturb.engineWrapSkew ? 1 : 0 );
			w.Reset( kCols, kRows );
			w.SetRule( turns );
			if( set == 0 )
				w.PlaceAnts( 1 );
			else if( set == 1 )
				w.PlaceAnts( 8 );
			else//four at the corners, each facing off the grid or along its edge
				w.SetAnts( { ant::Ant { 0, 0, 3 }, ant::Ant { kCols - 1, kRows - 1, 1 }, ant::Ant { 0, kRows - 1, 0 },
				             ant::Ant { kCols - 1, 0, 2 } } );
			w.Soup( ThresholdU32( 0.3 ), 7u + static_cast< uint32_t >( r ) );
			const std::vector< uint8_t > start = w.Grid();
			const std::vector< ant::Ant > startAnts = w.Ants();
			if( set == 0 )
			{
				//The soup: density 0.3, the colours 1..n-1 evenly.
				const int n = static_cast< int >( turns.size() );
				std::vector< int > count( static_cast< size_t >( n ), 0 );
				for( uint8_t c : start )
					++count[ c ];
				const double density = 1.0 - static_cast< double >( count[ 0 ] ) / start.size();
				const double each    = static_cast< double >( start.size() - count[ 0 ] ) / ( n - 1 );
				bool even            = true;
				for( int c = 1; c < n; ++c )
					even = even && std::fabs( count[ static_cast< size_t >( c ) ] - each ) < 0.2 * each;
				soupBad += !( std::fabs( density - 0.3 ) < 0.02 && even );
				soupWhat += fmt( " %s %.3f%s", turns.c_str(), density, even ? "" : " UNEVEN" );
			}
			Walk walk( kCols, kRows, turns );
			walk.colour = start;
			walk.ants   = asVectors( startAnts );
			for( int chunk = 0; chunk < 12; ++chunk )
			{
				w.Step( 250, false );
				for( int i = 0; i < 250; ++i )
					walk.Step();
				wrongCells += countDiff( w.Grid(), walk.colour ) + countDiff( w.Visited(), walk.visited ) + countDiff( w.Visits(), walk.visit );
				wrongAnts += !sameAnts( w.Ants(), walk.ants );
			}
			crossings += walk.crossings;
			w.Step( 3000, true );
			notBack += countDiff( w.Grid(), start ) + ( sameAnts( w.Ants(), startAnts ) ? 0 : 1 ) + ( w.Rounds() == 0 ? 0 : 1 );
		}
		Check( wrongCells == 0 && wrongAnts == 0 && notBack == 0 && crossings > 0,
		       fmt( "%-12s 157x93 soup, 3000 rounds of 1, 8 and 4 edge ants: %d cells and %d ant checkpoints differ over 36 "
		            "checkpoints; %lld edge crossings; 3000 back: %d cells or ants not where they started",
		            turns.c_str(), wrongCells, wrongAnts, crossings, notBack ) );
	}
	Check( soupBad == 0, fmt( "the soup at Density 0.3: within 0.02, the colours even to 20%%:%s", soupWhat.c_str() ) );
	return Verdict();
}

//===========================================================================
// --highway: through the plugin, the state texture IS the walk.
//===========================================================================
int runHighway( const Perturb& perturb )
{
	std::printf( "\n=== highway: through the plugin, the GPU's state texture is the walk, frame by frame, to Langton's highway\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		rig.plugin.SetDropLastRowForTest( perturb.dropRow );
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		manual( rig );
		rig.plugin.SetGridForTest( 160, 90 );
		rig.Render( 1 );//the seed: empty, one ant in the middle facing up
		const ant::Ant home = rig.plugin.World().Ants()[ 0 ];
		Walk walk( 160, 90, "RL" );
		walk.ants        = { asVector( home ) };
		int wrongFrames  = 0, wrongTexels = 0, black10k = -1, black11k = -1;
		for( int f = 1; f <= 24; ++f )
		{
			rig.Rounds( 500 );
			for( int i = 0; i < 500; ++i )
				walk.Step();
			const Cells texture = rig.Texture();
			const int wrong     = texelsWrong( texture, walk ) + ( sameAnts( rig.plugin.World().Ants(), walk.ants ) ? 0 : 1 );
			wrongTexels += wrong;
			wrongFrames += wrong > 0;
			if( f == 20 || f == 22 )
			{
				int black = 0;
				for( size_t i = 0; i < texture.size(); i += 2 )
					black += ( texture[ i ] & 0xFFu ) == 1u;
				( f == 20 ? black10k : black11k ) = black;
			}
		}
		const ant::Ant& a = rig.plugin.World().Ants()[ 0 ];
		Check( wrongFrames == 0 && black10k == 720 && black11k == 834 && a.x - home.x == -56 && a.y - home.y == -32,
		       fmt( "%dx%d  160x90, 24 frames of 500 rounds: %d frames (%d texels) differ from the harness's walk; %d and %d "
		            "black at 10,000 and 11,000 (720, 834); at 12,000 the ant is at (%d, %d) from its start (-56, -32)",
		            raster.w, raster.h, wrongFrames, wrongTexels, black10k, black11k, a.x - home.x, a.y - home.y ) );
	}
	return Verdict();
}

//===========================================================================
// --reverse: Reverse runs the walk back exactly, by the clock and by the round.
//===========================================================================
int runReverse( const Perturb& perturb )
{
	std::printf( "\n=== reverse: Reverse takes every rule back to its seed exactly, and forward again to where it was\n" );
	for( const Raster& raster : kRasters )
	{
		int bad = 0;
		std::string what;
		for( int r = 0; r < ant::kRuleCount; ++r )
		{
			Rig rig;
			rig.plugin.SetUnturnSignForTest( perturb.unturn ? -1 : 1 );
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			manual( rig );
			rig.plugin.SetGridForTest( 97, 55 );
			rig.Set( PT_RULE, static_cast< float >( r ) );
			rig.Set( PT_PATTERN, static_cast< float >( Pattern::Soup ) );
			rig.Set( PT_DENSITY, 0.3f );
			rig.Set( PT_ANTS, 4.0f );
			rig.Render( 1 );
			const auto start     = rig.TextureColours();
			const auto startAnts = rig.plugin.World().Ants();
			//Out by the clock: 2000 rounds a second for 1.5 s.
			rig.Set( PT_SPEED, ParamFromSpeed( 2000.0 ) );
			rig.Render( 90 );
			const long long out = rig.plugin.World().Rounds();
			const auto there     = rig.TextureColours();
			const auto thereAnts = rig.plugin.World().Ants();
			//Back by the clock for half the time, the rest by the round.
			rig.Set( PT_REVERSE, 1.0f );
			rig.Render( 45 );
			const long long byClock = out - rig.plugin.World().Rounds();
			rig.Set( PT_SPEED, 0.0f );
			rig.Rounds( rig.plugin.World().Rounds() );
			const int offHome  = countDiff( rig.TextureColours(), start ) + ( sameAnts( rig.plugin.World().Ants(), startAnts ) ? 0 : 1 );
			const bool zero    = rig.plugin.World().Rounds() == 0;
			//And out again.
			rig.Set( PT_REVERSE, 0.0f );
			rig.Rounds( out );
			const int offThere = countDiff( rig.TextureColours(), there ) + ( sameAnts( rig.plugin.World().Ants(), thereAnts ) ? 0 : 1 );
			const bool ok      = out >= 2900 && byClock >= 1400 && offHome == 0 && zero && offThere == 0;
			bad += !ok;
			what += fmt( " %s(+%lld -%lld)%s", ant::kRules[ r ].turns, out, byClock,
			             ok ? "" : fmt( "[%d off at the seed, %d off out again]", offHome, offThere ).c_str() );
		}
		//Langton's own: 12,000 rounds into the highway and back to nothing.
		{
			Rig rig;
			rig.plugin.SetUnturnSignForTest( perturb.unturn ? -1 : 1 );
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			manual( rig );
			rig.plugin.SetGridForTest( 160, 90 );
			rig.Render( 1 );
			const auto home = rig.plugin.World().Ants();
			rig.Rounds( 12000 );
			rig.Set( PT_REVERSE, 1.0f );
			rig.Rounds( 12000 );
			const auto colours = rig.TextureColours();
			const bool empty   = countNonZero( colours ) == 0 && sameAnts( rig.plugin.World().Ants(), home );
			bad += !empty;
			what += empty ? " RL-highway-and-back" : " RL-highway-NOT-BACK";
		}
		Check( bad == 0, fmt( "%dx%d  97x55 soup, 4 ants, out by the clock then back half by the clock and half by the "
		                      "round, then out again (rounds out, back by the clock):%s",
		                      raster.w, raster.h, what.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --clock-law (no GL): the step clock at any frame rate.
//===========================================================================
int runClockLaw( const Perturb& perturb )
{
	std::printf( "\n=== clock-law: rounds = floor( Speed x elapsed ) at any frame rate, in double\n" );
	struct Case
	{
		double speed, fps;
		bool jitter;
	};
	const Case cases[] = { { 13.37, 60.0, false }, { 613.7, 23.976, false }, { 6100.3, 59.94, true }, { 0.71, 30.0, true } };
	for( const Case& c : cases )
	{
		StepClock clock;
		long long total = 0;
		float floatOwed = 0.0f;
		double elapsed  = 0.0;
		int wrong = 0, near = 0;
		for( int f = 1; f <= 36000; ++f )
		{
			double dt = 1.0 / c.fps;
			if( c.jitter )
				dt *= 0.5 + hash01( static_cast< uint32_t >( f ), 17 );
			elapsed += dt;
			if( perturb.lawPerFrame )
			{
				//The wrong model: the owed fraction carried in float.
				floatOwed += static_cast< float >( c.speed * dt );
				const float whole = std::floor( floatOwed );
				floatOwed -= whole;
				total += static_cast< long long >( whole );
			}
			else
				total += clock.Advance( c.speed, dt, 1 << 20 );
			const double ideal = c.speed * elapsed;
			if( std::fabs( ideal - std::round( ideal ) ) < 1e-6 )
				++near;//too close to a whole round to call; not scored
			else if( total != static_cast< long long >( std::floor( ideal ) ) )
				++wrong;
		}
		Check( wrong == 0, fmt( "Speed %g at %g fps%s, 36000 frames (%.0f s): %d frames off floor( Speed x elapsed ), %d "
		                        "within 1e-6 of a whole round and not scored",
		                        c.speed, c.fps, c.jitter ? " jittered" : "", elapsed, wrong, near ) );
	}
	return Verdict();
}

//===========================================================================
// --clock: at Resolume's ~499 million ms, through the plugin.
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: a host clock at 499,000,000 ms (a float resolves 32 ms there) runs rounds exactly\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h, nullptr, false ) )
			return 1;
		rig.Set( PT_SPEED, 0.0f );
		rig.plugin.SetGridForTest( 64, 36 );
		rig.plugin.SetFloatClockForTest( perturb.clockFloat );
		rig.hostUnit    = 1000.0;//milliseconds
		rig.clockOffset = 499000.0;
		//The unit vote needs the wall clock to agree: the first frames are
		//paced in real time, as a host would pace them.
		int paced = 0;
		while( rig.plugin.ClockScale() == 0.0 && paced < 60 )
		{
			rig.Render( 1 );
			std::this_thread::sleep_for( std::chrono::microseconds( 16667 ) );
			++paced;
		}
		rig.Render( 2 );
		const bool voted     = rig.plugin.ClockScale() == 0.001;
		const double speed   = 13.37;
		rig.Set( PT_SPEED, ParamFromSpeed( speed ) );
		const double effective = SpeedFromParam( ParamFromSpeed( speed ) );
		const int m            = rig.frame;//the first frame that owes rounds
		const long long base   = rig.plugin.World().Rounds();
		int wrong = 0, near = 0;
		double worstDt = 0.0;
		std::string seen;
		for( int stop = 0; stop < 10; ++stop )
		{
			for( int f = 0; f < 60; ++f )
			{
				rig.Render( 1 );
				worstDt = std::max( worstDt, std::fabs( rig.plugin.LastDt() - 1.0 / 60.0 ) );
			}
			const int n         = rig.frame - 1;
			const double ideal  = effective * ( rig.TimeOf( n ) - rig.TimeOf( m - 1 ) );
			const long long got = rig.plugin.World().Rounds() - base;
			if( std::fabs( ideal - std::round( ideal ) ) < 1e-6 )
				++near;
			else if( got != static_cast< long long >( std::floor( ideal ) ) )
				++wrong;
			if( stop % 3 == 0 )
				seen += fmt( " %lld", got );
		}
		//Double at 4.99e8 ms: an ulp is 6e-8 ms, so a frame's dt is right to
		//1e-10 s. Elapsed time telescopes, so a float clock can land the
		//count right at a checkpoint; its dt (0 or 32 ms) cannot hide.
		Check( voted && wrong == 0 && near < 10 && worstDt <= 1e-9,
		       fmt( "%dx%d  the vote settled on ms (%s, after %d paced frames); every dt within %.1e s of 1/60 (bound "
		            "1e-9); Speed %.4f for 600 frames: the count is floor( Speed x elapsed ) at %d of %d checkpoints (%s ...)",
		            raster.w, raster.h, voted ? "yes" : "NO", paced, worstDt, effective, 10 - near - wrong, 10 - near, seen.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --coverage: the picture is the grid box-filtered.
//===========================================================================
int runCoverage( const Perturb& perturb )
{
	std::printf( "\n=== coverage: each pixel is the painted area it covers (Mono, no trail, no ants drawn)\n" );
	for( const Raster& raster : kRasters )
	{
		struct Case
		{
			int rows;
			float gap;
		};
		std::vector< Case > cases = { { raster.h / 8, 0.2f }, { 97, 0.0f }, { 97, 0.3f }, { raster.h * 2, 0.0f } };
		for( const Case& c : cases )
		{
			const Grid grid = GridFor( c.rows, raster.w, raster.h );
			Rig rig;
			rig.plugin.SetPointSampleForTest( perturb.pointSample );
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			manual( rig );
			rig.plugin.SetGridForTest( grid.cols, grid.rows );
			rig.Set( PT_RULE, 1.0f );//LLRR: three painted colours of four
			rig.Set( PT_PATTERN, static_cast< float >( Pattern::Soup ) );
			rig.Set( PT_DENSITY, 0.4f );
			rig.Set( PT_PALETTE, static_cast< float >( Palette::Mono ) );
			rig.Set( PT_TRAIL, 0.0f );
			rig.Set( PT_SHOW_ANTS, 0.0f );
			rig.Set( PT_GAP, c.gap );
			rig.Render( 1 );
			rig.Rounds( 2000 );//some of it walked on, so the visited cells are in it too
			const auto state  = rig.TextureColours();
			const Floats out  = rig.Output();
			const double sx = static_cast< double >( grid.cols ) / raster.w, sy = static_cast< double >( grid.rows ) / raster.h;
			const double inset = 0.5 * GapFromParam( c.gap );
			//Every edge the shader computes is a float at up to max( cols,
			//rows ): an ulp there, four edges, over the pixel's width in cells.
			const double maxCoord = std::max( grid.cols, grid.rows );
			const double ulp      = std::ldexp( 1.0, static_cast< int >( std::floor( std::log2( maxCoord ) ) ) - 23 );
			const double bound    = 4.0 * ulp / std::min( sx, sy ) + 4.0 * std::ldexp( 1.0, -23 );
			double worst          = 0.0;
			long long exact = 0, total = 0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const double lx = x * sx, hx = ( x + 1 ) * sx, ly = y * sy, hy = ( y + 1 ) * sy;
					double lit = 0.0;
					for( int cy = static_cast< int >( std::floor( ly ) ); cy < std::min( grid.rows, static_cast< int >( std::ceil( hy ) ) ); ++cy )
						for( int cx = static_cast< int >( std::floor( lx ) ); cx < std::min( grid.cols, static_cast< int >( std::ceil( hx ) ) ); ++cx )
						{
							if( state[ static_cast< size_t >( cy ) * grid.cols + cx ] == 0 )
								continue;
							const double wx = std::max( 0.0, std::min( hx, cx + 1 - inset ) - std::max( lx, cx + inset ) );
							const double wy = std::max( 0.0, std::min( hy, cy + 1 - inset ) - std::max( ly, cy + inset ) );
							lit += wx * wy;
						}
					const double expect = lit / ( sx * sy );
					for( int ch = 0; ch < 4; ++ch )
					{
						const double got = out[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + ch ];
						worst            = std::max( worst, std::fabs( got - expect ) );
						exact += got == static_cast< float >( expect );
						++total;
					}
				}
			Check( worst <= bound, fmt( "%dx%d  %dx%d cells (%.3f px each), Gap %.2f: worst %.2e (bound %.1e from float "
			                            "edges), %.1f%% of values exact",
			                            raster.w, raster.h, grid.cols, grid.rows, raster.h / static_cast< double >( grid.rows ),
			                            GapFromParam( c.gap ), worst, bound, 100.0 * static_cast< double >( exact ) / static_cast< double >( total ) ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --look: each cell is drawn the colour the rule and the look say: the
// palette's colour for its own, the trail's glow by the steps since an ant
// stood there, the ant's colour where an ant stands.
//===========================================================================
int runLook( const Perturb& perturb )
{
	std::printf( "\n=== look: a cell is its colour, plus the trail at exp( -steps since / Trail ), and an ant is the ant's colour\n" );
	const int w = 320, h = 180, cols = 32, rows = 18;//cells of exactly 10 x 10 pixels
	struct Case
	{
		double trail;
		bool ants;
	};
	for( const Case& c : { Case { 200.0, true }, Case { 0.0, false } } )
	{
		Rig rig;
		if( !rig.Init( w, h ) )
			return 1;
		manual( rig );
		rig.plugin.SetGridForTest( cols, rows );
		rig.Set( PT_RULE, 1.0f );//LLRR
		rig.Set( PT_ANTS, 3.0f );
		rig.Set( PT_PATTERN, static_cast< float >( Pattern::Soup ) );
		rig.Set( PT_DENSITY, 0.3f );
		rig.Set( PT_PALETTE, static_cast< float >( Palette::Phosphor ) );
		rig.Set( PT_GAP, 0.0f );
		rig.Set( PT_TRAIL, ParamFromTrail( c.trail ) );
		rig.Set( PT_SHOW_ANTS, c.ants ? 1.0f : 0.0f );
		rig.Render( 1 );
		rig.Rounds( 3000 );
		const Cells texture = rig.Texture();
		const Floats out    = rig.Output();
		const double trail  = TrailFromParam( ParamFromTrail( c.trail ) );
		const uint32_t now  = static_cast< uint32_t >( rig.plugin.World().Clock() );
		const auto& ants    = rig.plugin.World().Ants();
		const int n         = 4;
		double worst        = 0.0;
		int glowing = 0, antCells = 0;
		for( int cy = 0; cy < rows; ++cy )
			for( int cx = 0; cx < cols; ++cx )
			{
				const size_t i   = static_cast< size_t >( cy ) * cols + cx;
				const uint32_t r = texture[ i * 2 ], g = texture[ i * 2 + 1 ];
				double expect[ 4 ] = { 0, 0, 0, 0 };
				bool antHere       = false;
				for( const ant::Ant& a : ants )
					antHere = antHere || ( c.ants && a.x == cx && a.y == cy );
				if( antHere )
				{
					for( int k = 0; k < 3; ++k )
						expect[ k ] = kPhosphor.ant[ k ];
					expect[ 3 ] = 1.0;
					++antCells;
				}
				else
				{
					const int colour = static_cast< int >( r & 0xFFu );
					if( colour != 0 )
					{
						const double t = static_cast< double >( colour - 1 ) / ( n - 2 );
						for( int k = 0; k < 3; ++k )
							expect[ k ] = kPhosphor.first[ k ] + ( kPhosphor.last[ k ] - kPhosphor.first[ k ] ) * t;
						expect[ 3 ] = 1.0;
					}
					if( trail > 0.0 && ( r & 0x100u ) != 0u )
					{
						double age = static_cast< double >( now - g );
						if( perturb.lookPerRound )
							age /= static_cast< double >( ants.size() );
						const double glow = std::exp( -age / trail );
						for( int k = 0; k < 3; ++k )
							expect[ k ] += kPhosphor.trail[ k ] * glow;
						expect[ 3 ] = std::min( expect[ 3 ] + glow, 1.0 );
						glowing += glow > 0.01;
					}
				}
				//The pixel at the cell's middle: wholly inside the cell.
				const int px = cx * 10 + 5, py = cy * 10 + 5;
				for( int k = 0; k < 4; ++k )
				{
					const double got = out[ ( static_cast< size_t >( py ) * w + px ) * 4 + k ];
					worst            = std::max( worst, std::fabs( got - std::min( expect[ k ], 1.0 ) ) );
				}
			}
		Check( worst <= 2e-5 && ( c.trail > 0.0 ? glowing > 10 && antCells == 3 : antCells == 0 ),
		       fmt( "LLRR, 3 ants, Phosphor, Trail %g, Show Ants %s, 3000 rounds: every cell within %.1e of its colour "
		            "(bound 2e-5); %d cells glowing above 1%%, %d ants drawn",
		            c.trail, c.ants ? "on" : "off", worst, glowing, antCells ) );
	}

	//A field painted under a twelve-colour rule, then the Rule set to LLRR:
	//every cell is drawn as the colour the walk now reads it as, its colour
	//modulo four (a multiple of four is colour 0, and transparent), never as
	//a palette slot LLRR has no colour in.
	{
		Rig rig;
		if( !rig.Init( w, h ) )
			return 1;
		manual( rig );
		rig.plugin.SetGridForTest( cols, rows );
		rig.Set( PT_RULE, 4.0f );//LLRRRLRLRLLR
		rig.Set( PT_ANTS, 3.0f );
		rig.Set( PT_PATTERN, static_cast< float >( Pattern::Soup ) );
		rig.Set( PT_DENSITY, 0.6f );
		rig.Set( PT_PALETTE, static_cast< float >( Palette::Phosphor ) );
		rig.Set( PT_GAP, 0.0f );
		rig.Set( PT_TRAIL, ParamFromTrail( 0.0 ) );
		rig.Set( PT_SHOW_ANTS, 0.0f );
		rig.Render( 1 );
		rig.Rounds( 3000 );
		rig.Set( PT_RULE, 1.0f );//LLRR
		rig.Render( 1 );
		const Cells texture = rig.Texture();
		const Floats out    = rig.Output();
		const int n         = 4;
		double worst        = 0.0;
		int leftover = 0, cleared = 0;
		for( int cy = 0; cy < rows; ++cy )
			for( int cx = 0; cx < cols; ++cx )
			{
				const size_t i     = static_cast< size_t >( cy ) * cols + cx;
				const int raw      = static_cast< int >( texture[ i * 2 ] & 0xFFu );
				const int colour   = perturb.lookRawColour ? raw : raw % n;
				double expect[ 4 ] = { 0, 0, 0, 0 };
				leftover += raw >= n;
				cleared += raw >= n && raw % n == 0;
				if( colour != 0 )
				{
					const double t = static_cast< double >( colour - 1 ) / ( n - 2 );
					for( int k = 0; k < 3; ++k )
						expect[ k ] = kPhosphor.first[ k ] + ( kPhosphor.last[ k ] - kPhosphor.first[ k ] ) * t;
					expect[ 3 ] = 1.0;
				}
				const int px = cx * 10 + 5, py = cy * 10 + 5;
				for( int k = 0; k < 4; ++k )
				{
					const double got = out[ ( static_cast< size_t >( py ) * w + px ) * 4 + k ];
					worst            = std::max( worst, std::fabs( got - std::min( std::max( expect[ k ], 0.0 ), 1.0 ) ) );
				}
			}
		Check( worst <= 2e-5 && leftover > 20 && cleared > 2,
		       fmt( "LLRRRLRLRLLR for 3000 rounds of a soup, then LLRR: every cell within %.1e of its colour modulo 4 "
		            "(bound 2e-5); %d cells painted beyond LLRR's four colours, %d of them a multiple of four and clear",
		            worst, leftover, cleared ) );
	}
	return Verdict();
}

//===========================================================================
// --over-check: the clip's brightness is the ground, in the rule's colours,
// the right way round; Mix 0 is the clip.
//===========================================================================
int runOver( const Perturb& perturb )
{
	std::printf( "\n=== over: the clip's brightness, quantised into the rule's colours, is the ground; unmirrored; Mix 0 is the clip\n" );
	for( const Raster& raster : kRasters )
	{
		//Black, with grey boxes of brightness 0.10, 0.35, 0.60 and 0.85 placed
		//asymmetrically, and one saturated blue (luma 0.07; brightest 0.95).
		const int w = raster.w, h = raster.h;
		struct Box
		{
			double x0, y0, x1, y1;
			float r, g, b;
		};
		const Box boxes[] = { { 0.10, 0.60, 0.30, 0.90, 0.10f, 0.10f, 0.10f }, { 0.05, 0.10, 0.30, 0.45, 0.35f, 0.30f, 0.20f },
			                  { 0.62, 0.55, 0.70, 0.95, 0.60f, 0.55f, 0.50f }, { 0.40, 0.05, 0.52, 0.20, 0.85f, 0.85f, 0.85f },
			                  { 0.80, 0.10, 0.95, 0.30, 0.02f, 0.05f, 0.95f } };
		Floats card( static_cast< size_t >( w ) * h * 4, 0.0f );
		for( int y = 0; y < h; ++y )
			for( int x = 0; x < w; ++x )
			{
				float* o = &card[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
				o[ 3 ]   = 1.0f;
				for( const Box& b : boxes )
					if( x >= b.x0 * w && x < b.x1 * w && y >= b.y0 * h && y < b.y1 * h )
					{
						o[ 0 ] = b.r;
						o[ 1 ] = b.g;
						o[ 2 ] = b.b;
					}
			}
		const Grid grid = GridFor( 90, w, h );
		for( int rule : { 0, 1 } )
		{
			const int n = static_cast< int >( std::strlen( ant::kRules[ rule ].turns ) );
			auto levelAt = [ & ]( int cx, int cy ) {
				const int px   = ( ( 2 * cx + 1 ) * w ) / ( 2 * grid.cols );
				const int py   = ( ( 2 * cy + 1 ) * h ) / ( 2 * grid.rows );
				const float* t = &card[ ( static_cast< size_t >( py ) * w + px ) * 4 ];
				const double b = std::max( std::max( t[ 0 ], t[ 1 ] ), t[ 2 ] ) * t[ 3 ];
				return std::min( n - 1, static_cast< int >( std::floor( b * n ) ) );
			};
			Rig rig( true );
			rig.plugin.SetMirrorForTest( perturb.overMirrored );
			if( !rig.Init( w, h, &card ) )
				return 1;
			manual( rig );
			rig.Set( PT_RULE, static_cast< float >( rule ) );
			rig.plugin.SetGridForTest( grid.cols, grid.rows );
			rig.Render( 1 );//the first frame takes the clip
			const auto texture = rig.TextureColours();
			const auto& walk   = rig.plugin.World().Grid();
			std::vector< int > count( static_cast< size_t >( n ), 0 );
			int wrong = 0;
			for( int cy = 0; cy < grid.rows; ++cy )
				for( int cx = 0; cx < grid.cols; ++cx )
				{
					const int expect = levelAt( cx, cy );
					const size_t i   = static_cast< size_t >( cy ) * grid.cols + cx;
					++count[ static_cast< size_t >( expect ) ];
					wrong += walk[ i ] != expect || texture[ i ] != expect;
				}
			std::string levels;
			bool everyLevel = true;
			for( int k = 0; k < n; ++k )
			{
				levels += fmt( " %d", count[ static_cast< size_t >( k ) ] );
				everyLevel = everyLevel && count[ static_cast< size_t >( k ) ] > 20;
			}
			Check( wrong == 0 && everyLevel,
			       fmt( "%dx%d  %s, %dx%d cells: the clip's levels (cells at each:%s), %d cells disagree in the walk or the "
			            "texture (the blue box counts)",
			            w, h, ant::kRules[ rule ].name, grid.cols, grid.rows, levels.c_str(), wrong ) );
			if( rule == 1 )
			{
				rig.Set( PT_MIX, 0.0f );
				rig.Render( 1 );
				const Floats out = rig.Output();
				int differ       = 0;
				for( size_t i = 0; i < out.size(); ++i )
					differ += out[ i ] != card[ i ];
				Check( differ == 0, fmt( "%dx%d  Mix 0 returns the clip bit for bit (%d floats differ)", w, h, differ ) );
			}
		}
	}
	return Verdict();
}

//===========================================================================
// --prime: no audio event on the first frame after a clip trigger.
//===========================================================================
int runPrime( const Perturb& perturb )
{
	std::printf( "\n=== prime: loud audio already playing when the clip is triggered fires nothing on the trigger frame\n" );
	for( bool effect : { false, true } )
		for( const Raster& raster : kRasters )
		{
			Rig rig( effect );
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			manual( rig );
			rig.plugin.SetGridForTest( 64, 36 );
			rig.plugin.SetUnprimedForTest( perturb.primeOff );
			rig.Set( PT_AUDIO_BURST, 0.1f );
			const long long burst = BurstFromParam( 0.1f );
			rig.feed              = AudioFeed::Pulses;
			rig.Render( 100 );//1.67 s of beats
			const unsigned long long before = rig.plugin.Onsets();
			const long long roundsBefore    = rig.plugin.World().Rounds();
			//The trigger: the host's clock goes back to 0.02 s -- just after a
			//hit, the music still loud, which is the case that deafened the
			//fleet's unprimed analysers. The next hit is at 0.5 s, 29 frames on.
			rig.clockOffset = 0.02 - static_cast< double >( rig.frame ) / rig.fps;
			rig.Render( 1 );
			const unsigned long long atTrigger = rig.plugin.Onsets();
			rig.Render( 35 );
			const unsigned long long after = rig.plugin.Onsets();
			Check( before >= 3 && roundsBefore == burst * static_cast< long long >( before ) && atTrigger == before
			           && after == atTrigger + 1,
			       fmt( "%s %dx%d: %llu onsets from 4 beats before (%lld rounds: %lld each); %llu on the trigger frame; "
			            "%llu in the next 35 frames (one hit, at 0.5 s)",
			            effect ? "Over  " : "source", raster.w, raster.h, before, roundsBefore, burst, atTrigger - before,
			            after - atTrigger ) );
		}

	//Audio Reverse: one onset turns time round, and switching Audio Reverse
	//off hands it back to Reverse, whatever the onsets left it at.
	{
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		manual( rig );
		rig.plugin.SetGridForTest( 64, 36 );
		rig.Set( PT_AUDIO_REVERSE, 1.0f );
		rig.feed = AudioFeed::Pulses;
		int frames = 0;
		while( rig.plugin.Onsets() < 1 && frames < 120 )
		{
			rig.Render( 1 );
			++frames;
		}
		const bool turned = rig.plugin.TimeReversed();
		rig.Set( PT_AUDIO_REVERSE, 0.0f );
		rig.Render( 1 );
		const bool handedBack = !rig.plugin.TimeReversed();
		rig.Set( PT_REVERSE, 1.0f );
		rig.Render( 1 );
		const bool reverseRules = rig.plugin.TimeReversed();
		const bool expectBack   = !perturb.audioTurnKept;
		Check( rig.plugin.Onsets() == 1 && turned && handedBack == expectBack && reverseRules == expectBack,
		       fmt( "Audio Reverse on, %llu onset in %d frames: time %s; Audio Reverse off: time %s; Reverse on: time %s",
		            rig.plugin.Onsets(), frames, turned ? "backwards" : "forwards", handedBack ? "forwards" : "backwards",
		            reverseRules ? "backwards" : "forwards" ) );
	}
	return Verdict();
}

//===========================================================================
// --resize: the walk survives the host's raster changing.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: a resize of the same shape keeps the walk to the bit; a new shape keeps the overlap; and back\n" );
	for( bool effect : { false, true } )
	{
		Rig rig( effect );
		if( !rig.Init( 1280, 720 ) )
			return 1;
		rig.plugin.SetClearOnResizeForTest( perturb.resizeClears );
		if( !effect )
			rig.Set( PT_PATTERN, static_cast< float >( Pattern::Soup ) );
		if( !rig.Render( 40 ) )
			return 1;
		rig.Set( PT_SPEED, 0.0f );
		rig.Render( 1 );
		const Grid g0     = rig.plugin.CurrentGrid();
		const Cells start = rig.Texture();
		int painted       = 0;
		for( size_t i = 0; i < start.size(); i += 2 )
			painted += start[ i ] != 0u;
		//Same shape, a quarter of the size.
		rig.Resize( 320, 180 );
		rig.Render( 1 );
		const Grid g1   = rig.plugin.CurrentGrid();
		const bool same = g1 == g0 && rig.Texture() == start;
		//4:3: fewer columns, the middle kept.
		rig.Resize( 960, 720 );
		rig.Render( 1 );
		const Grid g2      = rig.plugin.CurrentGrid();
		const Cells narrow = rig.Texture();
		const int offset   = ( g0.cols - g2.cols ) / 2;
		int lostInOverlap  = 0;
		for( int y = 0; y < g2.rows; ++y )
			for( int x = 0; x < g2.cols; ++x )
				for( int k = 0; k < 2; ++k )
					lostInOverlap += narrow[ ( static_cast< size_t >( y ) * g2.cols + x ) * 2 + k ]
					                 != start[ ( static_cast< size_t >( y ) * g0.cols + x + offset ) * 2 + k ];
		//And back: the middle as it was, the margins empty.
		rig.Resize( 1280, 720 );
		rig.Render( 1 );
		const Cells wide = rig.Texture();
		int wrongBack    = 0;
		for( int y = 0; y < g0.rows; ++y )
			for( int x = 0; x < g0.cols; ++x )
				for( int k = 0; k < 2; ++k )
				{
					const size_t i        = ( static_cast< size_t >( y ) * g0.cols + x ) * 2 + k;
					const bool inside     = x >= offset && x < offset + g2.cols;
					const uint32_t expect = inside ? start[ i ] : 0u;
					wrongBack += wide[ i ] != expect;
				}
		Check( same && lostInOverlap == 0 && wrongBack == 0 && painted > 1000,
		       fmt( "%s: %dx%d grid, %d cells painted or walked: 1280x720 -> 320x180 %s; -> 960x720 (%dx%d) %d values of "
		            "the overlap changed; -> 1280x720 %d values not the middle as it was and empty margins",
		            effect ? "Over  " : "source", g0.cols, g0.rows, painted, same ? "bit-identical" : "CHANGED", g2.cols,
		            g2.rows, lostInOverlap, wrongBack ) );
	}
	return Verdict();
}

//===========================================================================
// --cues (no GL): options, booleans, events and integers step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options, booleans, events and integers, and ramps standard controls\n" );
	std::istringstream sheet( "0 Rule 0\n60 Rule 2\n0 Reverse 0\n30 Reverse 1\n"
	                          "0 Ants 2\n60 Ants 8\n0 Gap 0\n60 Gap 1\n0 Reseed 0\n30 Reseed 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	LangtonPlugin plugin( false );
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const int bFrame      = found->second.back().first;
		const float atB       = valueAt( found->second, bFrame, ramp );
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b && atB == b ) : ( mid == a && valueAt( found->second, bFrame - 1, ramp ) == a && atB == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	for( bool effect : { false, true } )
	{
		LangtonPlugin plugin( effect );
		std::map< std::string, int > seen, address;
		int longNames = 0, dupes = 0, clashes = 0;
		for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
		{
			const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
			if( name.size() > 16 )
			{
				std::printf( "    too long: %s\n", name.c_str() );
				++longNames;
			}
			if( seen[ name ]++ > 0 )
				++dupes;
			//Arena's OSC/REST address: lower case, spaces removed.
			std::string key;
			for( char c : name )
				if( c != ' ' )
					key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
			if( address[ key ]++ > 0 )
				++clashes;
		}
		//The About block is last, so a "User guide" button added later moves
		//no control's index.
		const unsigned int aboutFirst = plugin.ParamCount() - stoatworks::about::kParamCount;
		const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
		Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
		       fmt( "%s: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
		            effect ? "SW Langton Over" : "SW Langton", plugin.ParamCount(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	}
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back\n" );
	for( bool effect : { false, true } )
	{
		Rig rig( effect );
		if( !rig.Init( 320, 180 ) )
			return 1;
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 3; ++frame )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 7, 5, 300, 170 );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, 320, 180 );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin.SetTime( frame / 60.0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
			GLfloat clear[ 4 ]   = {};
			GLboolean mask[ 4 ]  = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 7 && viewport[ 1 ] == 5 && viewport[ 2 ] == 300 && viewport[ 3 ] == 170, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			for( int u = 0; u < 10; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		Check( problems == 0, fmt( "%s, three frames: viewport, vertex array, array buffer, program, active unit, framebuffer, "
		                           "blend, scissor, clear colour, colour mask, ten texture units (%d wrong:%s)",
		                           effect ? "Over" : "source", problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "literature", runLiterature, true }, { "engine", runEngine, true },       { "highway", runHighway, false },
		{ "reverse", runReverse, false },      { "clock-law", runClockLaw, true }, { "clock", runClock, false },
		{ "coverage", runCoverage, false },    { "look", runLook, false },         { "over", runOver, false },
		{ "prime", runPrime, false },          { "resize", runResize, false },     { "cues", runCues, true },
		{ "names", runNames, true },           { "state", runState, false },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	add( "literature", runLiterature, "read Langton's ant mirror-image (LR), and the pair rules with one letter changed", []( Perturb& p ) { p.literatureMirror = true; } );
	add( "engine", runEngine, "the torus wraps one cell short on the left and bottom", []( Perturb& p ) { p.engineWrapSkew = true; } );
	add( "highway", runHighway, "the upload skips the last row each frame touched", []( Perturb& p ) { p.dropRow = true; } );
	add( "reverse", runReverse, "the way back undoes each turn the wrong way", []( Perturb& p ) { p.unturn = true; } );
	add( "clock-law", runClockLaw, "the owed fraction carried in float", []( Perturb& p ) { p.lawPerFrame = true; } );
	add( "clock", runClock, "take elapsed time from the host clock in float", []( Perturb& p ) { p.clockFloat = true; } );
	add( "coverage", runCoverage, "point-sample the cell under each pixel's centre", []( Perturb& p ) { p.pointSample = true; } );
	add( "look", runLook, "expect the trail to fade by rounds, not ant steps", []( Perturb& p ) { p.lookPerRound = true; } );
	add( "look", runLook, "expect a cell painted under a longer rule drawn as its own colour", []( Perturb& p ) { p.lookRawColour = true; } );
	add( "over", runOver, "lay the clip under the grid mirrored", []( Perturb& p ) { p.overMirrored = true; } );
	add( "prime", runPrime, "run the analyser unprimed", []( Perturb& p ) { p.primeOff = true; } );
	add( "prime", runPrime, "expect an onset's turn to outlast Audio Reverse switched off", []( Perturb& p ) { p.audioTurnKept = true; } );
	add( "resize", runResize, "clear the walk on a resize", []( Perturb& p ) { p.resizeClears = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/langton.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 300;
	double fps = 60.0;
	bool beat = false, effect = false;
	std::string mode, scriptPath, clipPath;
	int filmFrames = -1;
	bool sizeGiven = false, framesGiven = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "lntest -- render Langton offline and measure its ants\n\n"
			             "  --out PATH        render and write a PNG (default /tmp/langton.png)\n"
			             "  --over            the Over effect, on the harness's night-street card\n"
			             "  --clip FILE       (with --over --out) a raw RGBA frame of --size to use as the clip\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 300: 5 s, 3000 rounds)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --beat            feed a beat every half second into the Audio buffer\n"
			             "  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA out: the source makes --frames N (0/absent: until the\n"
			             "                    reader hangs up); the Over effect takes frames in on stdin\n"
			             "  --film N          N frames, raw RGBA on stdout (the Over on its card)\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n"
			             "  --expect          a draft of the fleet Arena gate's expectation\n\n"
			             "  checks: --highway --reverse --clock --coverage --look --over-check --prime --resize --state\n"
			             "          no GL: --literature --engine --clock-law --cues --names\n"
			             "          --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
		{
			frames      = std::atoi( argv[ ++i ] );
			framesGiven = true;
		}
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--beat" )
			beat = true;
		else if( argument == "--over" )
			effect = true;
		else if( argument == "--clip" && hasNext )
			clipPath = argv[ ++i ];
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument == "--over-check" )
			mode = "over";
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "expect" )
	{
		//A DRAFT of the fleet gate's expectation, from what the plugins
		//really declare: defaults are the floats the constructors set, not
		//rounded literals (containment's trap).
		std::printf( "{\n  \"plugin\": \"langton\",\n  \"dlls\": [\"Langton.dll\", \"Langton Over.dll\"],\n"
		             "  \"register\": [\n    {\"name\": \"SW Langton\", \"uid\": \"LA01\", \"kind\": \"source\"},\n"
		             "    {\"name\": \"SW Langton Over\", \"uid\": \"LA02\", \"kind\": \"effect\"}\n  ],\n  \"params\": {\n" );
		for( bool over : { false, true } )
		{
			LangtonPlugin plugin( over );
			std::printf( "    \"%s\": [\n", over ? "SW Langton Over" : "SW Langton" );
			const std::vector< NamedParameter > list = listParameters( plugin );
			for( size_t i = 0; i < list.size(); ++i )
			{
				const NamedParameter& p = list[ i ];
				std::string line        = "      {\"name\": \"" + p.name + "\", ";
				if( p.type == FF_TYPE_OPTION )
					line += "\"type\": \"ParamChoice\", \"default\": \"" + std::string( plugin.GetParamElementName( p.index, static_cast< unsigned int >( std::lround( p.value ) ) ) ) + "\"";
				else if( p.type == FF_TYPE_BUFFER )
					line += "\"type\": \"ParamChoice\", \"default\": \"Composition\", \"requires\": \"audio\"";
				else if( p.type == FF_TYPE_BOOLEAN )
					line += std::string( "\"type\": \"ParamBoolean\", \"default\": " ) + ( p.value > 0.5f ? "true" : "false" );
				else if( p.type == FF_TYPE_EVENT )
					line += "\"type\": \"ParamEvent\"";
				else if( p.type == FF_TYPE_TEXT )
					line += "\"type\": \"ParamString\", \"default_pattern\": \"^Langton\\\\ v{version}\\\\ \\\\-\\\\ MIT\\\\ \\\\-\\\\ Stoatworks\\\\ Labs,\\\\ stoatworks\\\\-labs\\\\.com$\"";
				else
				{
					const float lo = p.type == FF_TYPE_INTEGER ? plugin.GetParamRange( p.index ).min : 0.0f;
					const float hi = p.type == FF_TYPE_INTEGER ? plugin.GetParamRange( p.index ).max : 1.0f;
					line += fmt( "\"type\": \"ParamRange\", \"min\": %.1f, \"max\": %.1f, \"default\": %.17g", lo, hi, static_cast< double >( p.value ) );
				}
				if( p.name.rfind( "Audio ", 0 ) == 0 )
					line += ", \"requires\": \"audio\"";
				if( p.name == "Speed" || p.name == "Step" || p.name == "Reverse" || p.name == "Rule" )
					line += ", \"note\": \"acts as the ants walk: single grabs of a moving field may read it inconclusive\"";
				if( p.name == "Density" || p.name == "Seed" || p.name == "Reseed" )
					line += ", \"note\": \"acts on a soup only (Pattern Soup), Density from the next reseed: at the "
					        "defaults a grab reads it inconclusive\"";
				line += i + 1 < list.size() ? "},\n" : "}\n";
				std::printf( "%s", line.c_str() );
			}
			std::printf( over ? "    ]\n" : "    ],\n" );
		}
		std::printf( "  }\n}\n" );
		return 0;
	}
	if( mode == "list" )
	{
		LangtonPlugin plugin( effect );
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		//No context at all: what a runner with no accelerated GL can run. The
		//skip is loud, so a green run is not read as one that checked pixels.
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The texture and picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n", failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		//The fleet's two shapes: an effect is frames in, frames out (toner's);
		//a source makes --frames of them, or runs until the reader hangs up
		//(pattern's).
		result = runPipe( effect, width, height, fps, scriptPath, framesGiven ? frames : 0, effect, beat, settings );
	else if( mode == "film" )
		result = runPipe( effect, width, height, fps, scriptPath, filmFrames, false, beat, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig( effect );
		rig.fps = fps;
		Floats card;
		if( effect && !clipPath.empty() )
		{
			std::ifstream file( clipPath, std::ios::binary );
			Bytes raw( static_cast< size_t >( width ) * height * 4 );
			if( !file.read( reinterpret_cast< char* >( raw.data() ), static_cast< std::streamsize >( raw.size() ) ) )
			{
				std::fprintf( stderr, "--clip %s: not %dx%d RGBA\n", clipPath.c_str(), width, height );
				return 2;
			}
			card.resize( raw.size() );
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					card[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = raw[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
		}
		else if( effect )
			card = buildCard( width, height );
		if( !rig.Init( width, height, effect ? &card : nullptr ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( beat )
				rig.feed = AudioFeed::Pulses;
			if( !rig.Render( std::max( frames, 1 ) ) )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s), round %lld\n", outPath.c_str(), width, height,
				             frames, fps, frames / fps, rig.plugin.World().Rounds() );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
