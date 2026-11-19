#pragma once
// A fake Ender's Echo that behaves like the real one (measured from an Elite Insights log):
//  - stands at the wall, casts (skill 47927) ~1.0 s after stopping,
//  - ~1.2 s after the cast runs in a straight line towards where its target stood at cast time,
//  - runs at ~400 in/s (~10.2 m/s) until it hits the arena wall, then repeats.
// The simulated arena is a circle centred where the player stood when the simulation started.
// With a single player, the "random target" is always the local player.

#include "EchoSource.h"

class SimulatedEcho : public IEchoSource
{
public:
	const char* Name() const override { return "Simulated"; }
	EchoState Update(const Vec3& aPlayer, float aDt) override;

	void Start(const Vec3& aPlayer);
	void Stop() { mActive = false; }
	bool IsActive() const { return mActive; }

	float ArenaRadius = 45.0f;   // metres
	float Speed       = 10.16f;  // metres / second
	float WaitToCast  = 1.0f;    // seconds at the wall before the cast
	float CastToRun   = 1.2f;    // seconds between cast and running

private:
	enum class Phase { Waiting, Casting, Running };

	bool  mActive = false;
	Phase mPhase  = Phase::Waiting;
	float mTimer  = 0;
	Vec3  mCenter;
	Vec3  mPos;
	Vec3  mDir;
};
