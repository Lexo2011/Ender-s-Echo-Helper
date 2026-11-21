#pragma once
// Agents reported by ArcDPS (relayed by Nexus). Realtime events arrive ~2-3 s late, but they
// tell us which in-game agent id (instid) belongs to which NPC species.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ArcAgents
{
	struct Agent
	{
		uint16_t    InstId   = 0;
		uint32_t    Species  = 0;  // NPC species id, 0 for players
		bool        IsSelf   = false;
		bool        IsNpc    = false;
		uint64_t    LastSeen = 0;  // GetTickCount64()
		std::string Name;
	};

	void Init();
	void Shutdown();

	uint64_t EventCount();
	std::unordered_map<uint16_t, Agent> Snapshot();

	/// Most recently seen agent of that species, or 0.
	uint16_t FindNewest(uint32_t aSpecies);

	/// True if an agent of that species was reported within the last aMilliseconds.
	bool SeenRecently(uint32_t aSpecies, uint64_t aMilliseconds);
}
