#pragma once

#include "EchoSource.h"
#include "Settings.h"

enum class Threat { None, Idle, Safe, Warn, Danger };

struct ThreatInfo
{
	Threat Level        = Threat::None;
	float  PathDistance = 0;  // perpendicular distance to the Echo's path (m)
	float  EchoDistance = 0;  // straight distance to the Echo (m)
	float  TimeToReach  = -1; // seconds until the Echo passes you, -1 if not approaching
};

ThreatInfo Evaluate(const Settings& aSettings, const Vec3& aPlayer, const EchoState& aEcho);

/// Draws the ground circle around the player and the arrow towards the Echo.
void DrawOverlay(const Settings& aSettings, const Camera& aCamera, const Vec3& aPlayer,
                 const EchoState& aEcho, const ThreatInfo& aThreat, const char* aSourceName);
