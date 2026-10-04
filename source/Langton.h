#pragma once

#include "Ant.h"
#include "Audio.h"
#include "Clock.h"
#include "Controls.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <cstdint>
#include <vector>

namespace langton
{
/**
    The plugin: the source (ants on an empty grid or a soup) and, with
    `isEffect`, the Over effect (the clip is the ground). One class, two
    registrations; see SourcePlugin.cpp and EffectPlugin.cpp.

    The walk is `ant::World`, on the CPU, exact. Each frame the rows it touched
    are uploaded into an RG32UI texture and the composite draws it.

    Host indices are NOT ParamIds: each plugin declares its own dense list,
    `HostOrder( isEffect )`. The host-facing calls translate; everything
    inside works in ParamIds.
*/
class LangtonPlugin : public CFFGLPlugin
{
public:
	explicit LangtonPlugin( bool isEffect );
	~LangtonPlugin() override;

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	/// The base class's stub fails, and a failed default deletes the instance
	/// -- so without this no real host can load the plugin.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	FFResult SetTime( double time ) override;

	bool IsEffect() const
	{
		return isEffect;
	}
	unsigned int ParamCount() const
	{
		return static_cast< unsigned int >( hostOrder.size() );
	}
	int HostIndexOf( unsigned int id ) const
	{
		return id < PT_COUNT ? idToHost[ id ] : -1;
	}
	void SetById( unsigned int id, float value );
	float GetById( unsigned int id ) const
	{
		return id < PT_COUNT ? params[ id ] : 0.0f;
	}

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these.
	//-------------------------------------------------------------------
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	double ClockScale() const
	{
		return clockScale;
	}
	/// A grid of exactly this size, whatever Cell Size and the raster say.
	void SetGridForTest( int cols, int rows )
	{
		gridOverride = Grid { cols, rows };
	}
	/// On the next frame, run exactly this many rounds more than the clock
	/// owes, with no per-frame cap, in the direction in force.
	void RequestRoundsForTest( long long rounds )
	{
		testRounds += rounds;
	}
	/// The walk itself, read-only: the harness compares it with its own.
	const ant::World& World() const
	{
		return world;
	}
	/// Exactly these ants, from the next frame (after any reseed).
	void SetAntsForTest( const std::vector< ant::Ant >& ants )
	{
		pendingAnts    = ants;
		antsForTest    = true;
	}
	/// --reverse's negative control: the undo turns the wrong way.
	void SetUnturnSignForTest( int sign )
	{
		world.SetUnturnSignForTest( sign );
	}
	/// --highway's negative control: the upload skips the last touched row.
	void SetDropLastRowForTest( bool on )
	{
		dropLastRowForTest = on;
	}
	/// --coverage's negative control: the cell under each pixel's centre.
	void SetPointSampleForTest( bool on )
	{
		pointSampleForTest = on;
	}
	/// --over-check's negative control: the clip laid under the grid mirrored.
	void SetMirrorForTest( bool on )
	{
		mirrorForTest = on;
	}
	/// --clock's negative control: elapsed time from the host clock in float.
	void SetFloatClockForTest( bool on )
	{
		floatClockForTest = on;
	}
	/// --resize's negative control: the walk cleared on a resize.
	void SetClearOnResizeForTest( bool on )
	{
		clearOnResizeForTest = on;
	}
	/// --prime's negative control: the analyser deaf on its first frame too.
	void SetUnprimedForTest( bool on )
	{
		unprimed = on;
	}

	GLuint StateTextureID() const
	{
		return state.TextureID();
	}
	Grid CurrentGrid() const
	{
		return grid;
	}
	unsigned long long Onsets() const
	{
		return onsetsUsed;
	}
	unsigned long long Reseeds() const
	{
		return reseeds;
	}
	double LastDt() const
	{
		return frameDt;
	}
	bool TimeReversed() const
	{
		return reversedNow;
	}

private:
	void UpdateClock();
	uint32_t seedSalt() const;
	void seed( const FFGLTextureStruct* input, int width, int height );
	void upload();
	void composite( const FFGLTextureStruct* input, const GLint* hostViewport, GLuint hostFBO, int width,
	                int height );

	const bool isEffect;
	const std::vector< unsigned int >& hostOrder;
	int idToHost[ PT_COUNT ];
	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader sampleShader, compositeShader;
	ffglex::FFGLScreenQuad quad;
	PassBuffer state, sampleTarget;
	GLuint blankClip = 0;
	std::vector< uint32_t > staging;

	ant::World world;
	Grid grid;
	Grid gridOverride;

	//Time. Resolume has sent both seconds and milliseconds (see millpond).
	double hostTime = -1.0, lastRawTime = -1.0, lastWallTime = -1.0, wallStart = -1.0;
	double clockScale = 0.0;
	int secondsVotes = 0, millisVotes = 0;
	double now = 0.0, lastNow = -1.0;
	bool settledJump = false;
	double frameDt   = 0.0;
	StepClock clock;

	bool needSeed       = true;
	uint32_t seedSerial = 0;
	int lastPattern = -1, lastSeed = -1, lastRule = -1, lastAnts = -1;
	bool stepHeld = false, reseedHeld = false;
	long long pendingRounds = 0;
	long long testRounds    = 0;
	bool audioReversed      = false;
	bool reversedNow        = false;
	std::vector< ant::Ant > pendingAnts;
	bool antsForTest = false;
	int lastWidth = 0, lastHeight = 0;
	unsigned long long reseeds = 0;

	audio::Analyser analyser;
	bool unprimed = false;
	unsigned long long onsetsUsed = 0;

	bool dropLastRowForTest   = false;
	bool pointSampleForTest   = false;
	bool mirrorForTest        = false;
	bool floatClockForTest    = false;
	bool clearOnResizeForTest = false;
};

} // namespace langton
