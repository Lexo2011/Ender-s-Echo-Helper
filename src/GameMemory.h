#pragma once
// Safe reads of the game's memory and the layout found with the diagnostic.
// All reads go through ReadProcessMemory: a stale address fails instead of crashing the game.

#include "Geometry.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace GameMemory
{
	/// Character layout for one game build (found with the memory diagnostic).
	struct Layout
	{
		unsigned  Build;
		uintptr_t CharVtableRva;   // position object type
		int       PosOffset;       // float[3], inches, stored (x, z, -y)
		int       AgentPtrOffset;  // pointer to the agent object
		int       AgentIdOffset;   // uint32 agent id (= ArcDPS instid) inside the agent object
		int       SpeciesPath[3];  // optional: species id via pointer hops, {-1} if unknown
		int       SmoothHops[3];   // agent -> movement object: [[[agent+h0]+h1]+h2]
		int       SmoothPosOffset; // float[3] per-frame position in the movement object (inches, (x, z, -y))
	};

	/// Reads the agent's per-frame (smooth) position through the layout's movement-object route.
	bool ReadSmoothPos(const Layout& aLayout, uintptr_t aAgent, Vec3& aOut);
	/// Address of the agent's movement object (end of the smooth route), or 0.
	uintptr_t MovementObject(const Layout& aLayout, uintptr_t aAgent);
	/// Offsets (relative to the movement object) of the six copies of the per-frame position.
	constexpr int MOVE_COPIES[6] = { 0x6D4, 0x714, 0x720, 0x760, 0x7A0, 0x7AC };
	/// Type marker inside movement objects (build 210545): vtable exe+0x21272D8 at +0x5B8.
	constexpr uintptr_t MOVE_MARKER_RVA    = 0x21272D8;
	constexpr int       MOVE_MARKER_OFFSET = 0x5B8;

	/// The layout for the running build, or nullptr if this build hasn't been mapped yet.
	const Layout* CurrentLayout(unsigned aBuild);

	void Init();
	uintptr_t ExeBase();
	bool InExe(uintptr_t p);

	bool Read(uintptr_t aAddr, void* aOut, size_t aSize);
	template <typename T> bool Read(uintptr_t aAddr, T& aOut) { return Read(aAddr, &aOut, sizeof(T)); }

	bool LooksLikeHeapPtr(uint64_t q);

	/// Stored game position (inches, (x, z, -y)) -> MumbleLink metres (x, y-up, z).
	inline Vec3 GameToMumble(const float* f)
	{
		return Vec3{ f[0], -f[2], f[1] } * INCH_TO_METRE;
	}

	struct Region { uintptr_t Base; size_t Size; };
	std::vector<Region> ScannableRegions();

	/// Finds all 8-byte aligned addresses in the given regions whose qword equals aValue.
	/// aCancel is polled between chunks.
	template <typename Cancel>
	void FindQword(const std::vector<Region>& aRegions, uint64_t aValue, std::vector<uintptr_t>& aOut,
	               std::vector<Region>* aHitRegions, Cancel&& aCancel)
	{
		constexpr size_t CHUNK = 1 << 20;
		std::vector<uint8_t> buf(CHUNK);
		for (auto& r : aRegions)
		{
			bool hit = false;
			for (size_t off = 0; off < r.Size; off += CHUNK)
			{
				if (aCancel()) { return; }
				size_t n = (r.Size - off) < CHUNK ? (r.Size - off) : CHUNK;
				if (!Read(r.Base + off, buf.data(), n)) { continue; }
				const uint64_t* q = (const uint64_t*)buf.data();
				for (size_t i = 0; i < n / 8; i++)
				{
					if (q[i] == aValue) { aOut.push_back(r.Base + off + i * 8); hit = true; }
				}
			}
			if (hit && aHitRegions) { aHitRegions->push_back(r); }
		}
	}
}
