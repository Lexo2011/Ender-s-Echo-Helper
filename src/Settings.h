#pragma once

#include <string>

struct Settings
{
	bool  Enabled           = true;
	bool  OnlyInHallOfChains = true;  // hide outside map 1264 (simulation ignores this)
	bool  ShowStatusText    = true;
	bool  ShowArrowDistance = true;   // "12 m" next to the arrow tip
	bool  ShowPath          = true;   // the Echo's dash drawn on the ground
	int   PathStyle         = 1;      // 0 = line, 1 = band on the floor (danger/warning width)
	float PathThickness     = 3.0f;   // pixels
	float ColPath[4]        = { 1.00f, 0.45f, 0.10f, 0.80f };

	// Testing: track this NPC species instead of Ender's Echo (0 = off). Works on any map.
	int   TestSpecies       = 0;

	// Squad marker: put the X (Cross) agent marker on the Echo once per attempt.
	bool  AutoMark          = false;
	bool  AutoMarkOnlyAsCommander = true; // off = also try as lieutenant (cannot be detected)

	float CircleRadius      = 1.5f;   // metres, drawn around the player
	float CircleThickness   = 3.0f;   // pixels
	float ArrowLength       = 1.2f;   // metres beyond the circle
	float ArrowWidth        = 0.8f;   // metres at the base

	// Distances from the Echo's path (perpendicular, on the ground) in metres.
	float DangerHalfWidth   = 2.5f;   // closer than this -> red
	float WarnHalfWidth     = 6.0f;   // closer than this -> yellow, otherwise green
	float MovingThreshold   = 1.0f;   // m/s; slower counts as standing at the wall

	float ColDanger[4] = { 0.95f, 0.15f, 0.15f, 0.90f };
	float ColWarn[4]   = { 1.00f, 0.85f, 0.10f, 0.90f };
	float ColSafe[4]   = { 0.20f, 0.90f, 0.30f, 0.80f };
	float ColIdle[4]   = { 0.70f, 0.75f, 0.85f, 0.70f };  // Echo standing still: next target unknown

	void Load(const std::string& aPath);
	void Save(const std::string& aPath) const;
};
