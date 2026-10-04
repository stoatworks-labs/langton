#pragma once

#include <algorithm>
#include <cmath>

/**
    When the ants step. No GL, so `lntest --offline` checks it in CI.

    The host's elapsed time, in DOUBLE, times Speed is owed steps; each frame
    runs the whole ones and carries the fraction. So after any number of
    frames at any frame rate the count is floor( Speed x elapsed ) -- to a
    double's rounding, which is 1e-12 of a step over hours -- and never drifts
    the way a per-frame float multiply would. (conway's clock, unchanged but
    for the noun: there it owes generations.)

    Resolume's clock has been measured at ~499 million ms, where a float
    resolves 32 ms: the elapsed time is taken frame to frame in double by the
    plugin (Langton.cpp), never from the host's absolute value as a float.
*/
namespace langton
{
class StepClock
{
public:
	/// Owe Speed x dt more steps and take the whole ones, at most `maxSteps`
	/// (the rest is dropped, not banked: a host at 2 fps does not earn a burst
	/// of half a second's steps on its next frame).
	long long Advance( double stepsPerSecond, double dt, long long maxSteps )
	{
		if( stepsPerSecond <= 0.0 || dt <= 0.0 )
			return 0;
		owed += stepsPerSecond * dt;
		double whole = std::floor( owed );
		if( whole > static_cast< double >( maxSteps ) )
		{
			whole = static_cast< double >( maxSteps );
			owed  = std::min( owed - whole, 0.999999 );
		}
		else
			owed -= whole;
		return static_cast< long long >( whole );
	}

	/// How far into the next step the clock is, 0..1.
	double Fraction() const
	{
		return owed;
	}

	void Reset()
	{
		owed = 0.0;
	}

private:
	double owed = 0.0;
};

} // namespace langton
