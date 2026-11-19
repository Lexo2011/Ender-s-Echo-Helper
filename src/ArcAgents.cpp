#include "ArcAgents.h"

#include "Shared.h"

#include <atomic>
#include <mutex>

namespace ArcAgents
{
	namespace
	{
		struct CbtEvent
		{
			uint64_t Time, SrcAgent, DstAgent;
			int32_t  Value, BuffDmg;
			uint32_t OverstackValue, SkillId;
			uint16_t SrcInstId, DstInstId, SrcMasterInstId, DstMasterInstId;
			uint8_t  Iff, Buff, Result, IsActivation, IsBuffRemove, IsNinety, IsFifty, IsMoving,
			         IsStatechange, IsFlanking, IsShields, IsOffcycle, Pad61, Pad62, Pad63, Pad64;
		};
		struct Ag { const char* Name; uintptr_t Id; uint32_t Prof; uint32_t Elite; uint32_t Self; uint16_t Team; };
		struct EvCombatData { CbtEvent* Ev; Ag* Src; Ag* Dst; const char* SkillName; uint64_t Id; uint64_t Revision; };

		std::mutex                          Mutex;
		std::unordered_map<uint16_t, Agent> Agents;
		std::atomic<uint64_t>               Events{ 0 };

		void Remember(const Ag* a, uint16_t aInstId)
		{
			if (!a || aInstId == 0) { return; }
			const bool notPlayer = a->Elite == 0xFFFFFFFF;
			const bool gadget    = notPlayer && (a->Prof & 0xFFFF0000) == 0xFFFF0000;
			if (gadget) { return; }

			std::lock_guard<std::mutex> l(Mutex);
			Agent& r  = Agents[aInstId];
			r.InstId  = aInstId;
			r.IsSelf  = a->Self != 0;
			r.IsNpc   = notPlayer;
			r.Species = notPlayer ? a->Prof : 0;
			r.LastSeen = GetTickCount64();
			if (a->Name) { r.Name = a->Name; }
		}

		void OnCombat(void* aPayload)
		{
			auto d = (EvCombatData*)aPayload;
			if (!d || !d->Ev) { return; }
			Events++;
			Remember(d->Src, d->Ev->SrcInstId);
			Remember(d->Dst, d->Ev->DstInstId);
		}
	}

	// "SQUAD" = all area events, "LOCAL" = chat box events. Both are raised by Nexus when ArcDPS is loaded.
	void Init()
	{
		APIDefs->Events_Subscribe("EV_ARCDPS_COMBATEVENT_SQUAD_RAW", OnCombat);
		APIDefs->Events_Subscribe("EV_ARCDPS_COMBATEVENT_LOCAL_RAW", OnCombat);
	}

	void Shutdown()
	{
		APIDefs->Events_Unsubscribe("EV_ARCDPS_COMBATEVENT_SQUAD_RAW", OnCombat);
		APIDefs->Events_Unsubscribe("EV_ARCDPS_COMBATEVENT_LOCAL_RAW", OnCombat);
	}

	uint64_t EventCount() { return Events; }

	std::unordered_map<uint16_t, Agent> Snapshot()
	{
		std::lock_guard<std::mutex> l(Mutex);
		return Agents;
	}

	uint16_t FindNewest(uint32_t aSpecies)
	{
		std::lock_guard<std::mutex> l(Mutex);
		uint16_t best = 0;
		uint64_t when = 0;
		for (auto& [id, a] : Agents)
		{
			if (a.IsNpc && a.Species == aSpecies && a.LastSeen >= when) { best = id; when = a.LastSeen; }
		}
		return best;
	}
}
