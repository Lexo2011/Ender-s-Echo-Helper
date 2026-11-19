#pragma once
// Memory diagnostic: locates where the game stores character positions.
//
// 1. Scan:    search readable private memory for the local player's MumbleLink position,
//             trying several encodings (metres / inches, different axis orders).
// 2. Filter:  after the player moved, keep only candidates that followed.
// 3. Analyze: for surviving candidates, look backwards for an object start (a qword pointing
//             into the game executable = vtable), then count all objects with the same vtable
//             and check whether their positions look like characters around the player.
//
// Everything is read with ReadProcessMemory, so stale addresses fail instead of crashing.
// Results go to the window and to addons/EndersEcho/diagnostic.txt.

#include "Geometry.h"

#include <string>

namespace Diagnostics
{
	void Init(const std::string& aLogPath);
	void Shutdown();          // stops the worker thread; call before the DLL unloads

	void RenderWindow(bool* aOpen, const Vec3& aPlayer);
}
