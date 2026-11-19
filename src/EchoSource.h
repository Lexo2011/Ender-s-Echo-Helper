#pragma once
// Where the overlay gets Ender's Echo from. The simulated source is used for testing;
// a live (game memory) source will implement the same interface.

#include "Geometry.h"

struct EchoState
{
	bool Valid    = false; // Echo exists and its position is known.
	Vec3 Position;         // MumbleLink space, metres.
	Vec3 Velocity;         // metres / second; ~0 while it stands at a wall.
	bool HasPathEnd = false;
	Vec3 PathEnd;          // where the current dash ends (arena edge), when known
};

class IEchoSource
{
public:
	virtual ~IEchoSource() = default;
	virtual const char* Name() const = 0;
	/// Called once per frame. aPlayer is the local player's position, aDt the frame time in seconds.
	virtual EchoState Update(const Vec3& aPlayer, float aDt) = 0;

	/// Preferred auto-mark path: put the Cross (X) agent marker on the Echo directly through the
	/// game, without touching the player's target. Returns true if the marker was placed.
	virtual bool CanMarkDirectly() const { return false; }
	virtual bool MarkEchoDirectly() { return false; }

	/// Fallback auto-mark path: the game's "Set Agent Marker" keybind only marks the current
	/// target, so this reports when the local player has Ender's Echo targeted.
	virtual bool IsEchoTargeted() const { return false; }
	virtual bool CanDetectTarget() const { return false; }
};
