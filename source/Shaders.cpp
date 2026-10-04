#include "Shaders.h"

namespace langton::shaders
{
const char* const kVersion = "#version 410 core\n";

//===========================================================================
// The library. No #version, no main.
//===========================================================================
const char* const kCommon = R"(
const uint COLOUR_MASK = 0xFFu;
const uint VISITED_BIT = 0x100u;
)";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//===========================================================================
// Sample (Over): the clip at each cell's centre, quantised into the rule's
// colours. The pixel is found in INTEGER arithmetic -- floor( ( c + 1/2 ) x
// raster / grid ) as ( 2c + 1 ) raster / ( 2 grid ) -- so which pixel decides
// a cell is exact on any GPU (conway's). Brightness is the brightest channel
// times alpha, not luma (radar's lesson: a saturated blue has luma 0.07).
// Level k of n covers brightness [ k/n, (k+1)/n ).
//===========================================================================
const char* const kSampleFragment = R"(
uniform sampler2D Clip;
uniform ivec2 ClipRaster;
uniform ivec2 Grid;
uniform int Colours;
uniform int MirrorForTest;
out uint Level;

void main()
{
	ivec2 c = ivec2( gl_FragCoord.xy );
	ivec2 p = ( ( 2 * c + 1 ) * ClipRaster ) / ( 2 * Grid );
	if( MirrorForTest == 1 )
		p.x = ClipRaster.x - 1 - p.x;
	vec4 t  = texelFetch( Clip, p, 0 );
	float b = clamp( max( max( t.r, t.g ), t.b ) * t.a, 0.0, 1.0 );
	Level   = min( uint( Colours - 1 ), uint( floor( b * float( Colours ) ) ) );
}
)";

//===========================================================================
// Composite. Each output pixel is the cell grid BOX-FILTERED over its
// footprint, every cell it overlaps weighted by the exact area of overlap
// with the cell's lit square (the cell inset by Gap / 2 each side): conway's
// composite. A cell's colour is premultiplied: colour 0 is nothing (the source
// is transparent there), colour k the palette's k; a cell an ant stood on
// glows in TrailColour at exp( -age / Trail ), age in ant steps on the
// activity clock (uint subtraction, so it survives the clock wrapping); an
// ant's own cell is AntColour, whole.
//===========================================================================
const char* const kCompositeFragment = R"(
uniform usampler2D State;
uniform sampler2D InputTexture;
uniform ivec2 Grid;
uniform vec2 Raster;
uniform vec2 ViewOrigin;
uniform float Gap;
uniform uint Now;
uniform float Trail;
uniform vec3 Palette[ 12 ];
uniform vec3 TrailColour;
uniform vec3 AntColour;
uniform ivec2 Ants[ 8 ];
uniform int AntCount;
uniform int IsEffect;
uniform float ClipColour;
uniform float Backdrop;
uniform float MixAmount;
uniform int PointSampleForTest;
out vec4 FragColor;

const int kSpan = 12;

vec4 cellAt( ivec2 c )
{
	for( int i = 0; i < AntCount; ++i )
		if( Ants[ i ] == c )
			return vec4( AntColour, 1.0 );
	uvec2 s     = texelFetch( State, c, 0 ).rg;
	uint colour = s.r & COLOUR_MASK;
	vec4 v      = colour == 0u ? vec4( 0.0 ) : vec4( Palette[ min( colour, 11u ) ], 1.0 );
	if( Trail > 0.0 && ( s.r & VISITED_BIT ) != 0u )
	{
		float glow = exp( -float( Now - s.g ) / Trail );
		v          = vec4( v.rgb + TrailColour * glow, min( v.a + glow, 1.0 ) );
	}
	return v;
}

void main()
{
	vec2 p     = gl_FragCoord.xy - ViewOrigin;//the pixel's centre
	vec2 scale = vec2( Grid ) / Raster;      //cells per pixel
	vec4 life  = vec4( 0.0 );
	if( PointSampleForTest == 1 )
	{
		//The negative control: the cell under the pixel's centre, unfiltered.
		life = cellAt( clamp( ivec2( floor( p * scale ) ), ivec2( 0 ), Grid - 1 ) );
	}
	else
	{
		vec2 lo  = ( p - 0.5 ) * scale;
		vec2 hi  = ( p + 0.5 ) * scale;
		ivec2 c0 = clamp( ivec2( floor( lo ) ), ivec2( 0 ), Grid - 1 );
		ivec2 c1 = clamp( ivec2( ceil( hi ) ) - 1, c0, min( Grid - 1, c0 + kSpan - 1 ) );
		float inset = 0.5 * Gap;
		vec4 sum    = vec4( 0.0 );
		for( int y = c0.y; y <= c1.y; ++y )
			for( int x = c0.x; x <= c1.x; ++x )
			{
				vec2 a = max( lo, vec2( x, y ) + inset );
				vec2 b = min( hi, vec2( x + 1, y + 1 ) - inset );
				vec2 w = max( b - a, vec2( 0.0 ) );
				float area = w.x * w.y;
				if( area > 0.0 )
					sum += area * cellAt( ivec2( x, y ) );
			}
		life = sum / ( scale.x * scale.y );
	}
	life = min( life, vec4( 1.0 ) );

	if( IsEffect == 0 )
	{
		//Premultiplied: where nothing is painted the source is transparent.
		FragColor = life;
		return;
	}
	//The clip's own texel, fetched, not filtered: Mix 0 hands it back bit for bit.
	vec4 clip = texelFetch( InputTexture, ivec2( p ), 0 );
	life.rgb  = mix( life.rgb, clip.rgb * life.a, ClipColour );
	vec3 over = life.rgb + clip.rgb * Backdrop * ( 1.0 - life.a );
	FragColor = vec4( mix( clip.rgb, over, MixAmount ), mix( clip.a, 1.0, MixAmount ) );
}
)";

std::string Assemble( const char* piece )
{
	std::string s = kVersion;
	s += kCommon;
	s += piece;
	return s;
}

} // namespace langton::shaders
