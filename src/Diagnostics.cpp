#include "Diagnostics.h"

#include "ArcAgents.h"
#include "GameMemory.h"
#include "Shared.h"
#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Diagnostics
{
	namespace
	{
		// ---------------------------------------------------------------- encodings

		// Scale from metres to the stored unit.
		constexpr float SCALES[] = { 1.0f, 1.0f / INCH_TO_METRE };
		constexpr const char* SCALE_NAMES[] = { "m", "in" };

		// How the three stored floats map onto MumbleLink (x, y-up, z).
		constexpr const char* PATTERN_NAMES[] = {
			"(x, y, z)",       // same as MumbleLink
			"(x, z, -y)",      // z-down height, like ArcDPS / EVTC
			"(x, z, y)",       // z-up height
			"(x, -z, y)",
		};
		constexpr int PATTERNS = 4;
		constexpr int ENCODINGS = 2 * PATTERNS;

		Vec3 Decode(const float* f, int aEnc)
		{
			float s = SCALES[aEnc / PATTERNS];
			Vec3 v;
			switch (aEnc % PATTERNS)
			{
				case 0: v = { f[0],  f[1],  f[2] }; break;
				case 1: v = { f[0], -f[2],  f[1] }; break;
				case 2: v = { f[0],  f[2],  f[1] }; break;
				case 3: v = { f[0],  f[2], -f[1] }; break;
			}
			return v * (1.0f / s);
		}

		std::string EncName(int aEnc)
		{
			return std::string(PATTERN_NAMES[aEnc % PATTERNS]) + " " + SCALE_NAMES[aEnc / PATTERNS];
		}

		constexpr float  TOLERANCE_M    = 0.15f;

		/// Character position type found by an earlier analysis. Vtable RVAs only hold for one game build.
		struct KnownType { unsigned Build; uintptr_t VtableRva; int Offset; int Enc; };
		constexpr KnownType KNOWN_TYPES[] = {
			{ 210545, 0x2124600, 0x108, 1 * PATTERNS + 1 }, // (x, z, -y) inches, found 2026-11-19
		};
		constexpr size_t MAX_CANDIDATES = 200000;
		constexpr size_t CHUNK          = 1 << 20;

		// ---------------------------------------------------------------- memory helpers

		HANDLE    Self       = nullptr;
		uintptr_t ExeBase    = 0;
		uintptr_t ExeEnd     = 0;
		uintptr_t ExeCodeEnd = 0;

		bool Read(uintptr_t aAddr, void* aOut, size_t aSize)
		{
			SIZE_T got = 0;
			return ReadProcessMemory(Self, (LPCVOID)aAddr, aOut, aSize, &got) && got == aSize;
		}

		template <typename T> bool Read(uintptr_t aAddr, T& aOut) { return Read(aAddr, &aOut, sizeof(T)); }

		bool InExe(uintptr_t p) { return p >= ExeBase && p < ExeEnd; }

		/// A vtable lives in the exe and its first entry points into the exe's code.
		bool LooksLikeVtable(uintptr_t p)
		{
			if (!InExe(p) || (p & 7)) { return false; }
			uintptr_t fn = 0;
			return Read(p, fn) && fn >= ExeBase && fn < ExeCodeEnd;
		}

		struct Region { uintptr_t Base; size_t Size; };

		std::vector<Region> ScannableRegions()
		{
			std::vector<Region> out;
			MEMORY_BASIC_INFORMATION mbi;
			uintptr_t addr = 0x10000;
			while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
			{
				bool rw = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
				bool bad = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
				if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && !bad)
				{
					out.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize });
				}
				uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
				if (next <= addr) { break; }
				addr = next;
			}
			return out;
		}

		/// Calls aFn(chunkBase, data, size) for every readable chunk of private memory.
		template <typename F>
		void ForEachChunk(std::atomic<float>& aProgress, std::atomic<bool>& aCancel, F&& aFn)
		{
			auto regions = ScannableRegions();
			size_t total = 0, done = 0;
			for (auto& r : regions) { total += r.Size; }

			std::vector<uint8_t> buf(CHUNK + 16);
			for (auto& r : regions)
			{
				for (size_t off = 0; off < r.Size; off += CHUNK)
				{
					if (aCancel) { return; }
					size_t n = std::min(CHUNK, r.Size - off);
					if (Read(r.Base + off, buf.data(), n)) { aFn(r.Base + off, buf.data(), n); }
					done += n;
					aProgress = total ? (float)done / total : 1.0f;
				}
			}
		}

		// ---------------------------------------------------------------- state

		struct Candidate { uintptr_t Addr; int Enc; };

		struct GroupResult
		{
			uintptr_t VtableRva   = 0;
			int       Offset      = 0;   // position offset inside the object
			int       Enc         = 0;
			size_t    Objects     = 0;   // objects with this vtable
			size_t    Plausible   = 0;   // of those, positions within 200 m of the player
			std::vector<float> Nearest; // distances of the closest objects (m)
		};

		std::mutex               Mutex;      // guards everything below that the UI reads
		std::vector<Candidate>   Candidates;
		std::vector<GroupResult> Groups;
		std::string              Status = "Idle.";
		std::string              LogPath;

		std::thread              Worker;
		std::atomic<bool>        Busy{ false };
		std::atomic<bool>        Cancel{ false };
		std::atomic<float>       Progress{ 0 };
		int                      FilterRounds = 0;

		void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(Mutex); Status = s; }

		void Log(const std::string& aLine)
		{
			std::ofstream f(LogPath, std::ios::app);
			f << aLine << '\n';
		}

		std::string Hex(uintptr_t v) { char b[32]; std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v); return b; }

		void LogHeader(const char* aWhat, const Vec3& aPlayer)
		{
			char t[64];
			std::time_t now = std::time(nullptr);
			std::strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
			char b[512];
			std::snprintf(b, sizeof(b), "\n=== %s  %s | build %u | map %u | exe %s size %s | player m (%.3f, %.3f, %.3f)",
				aWhat, t,
				MumbleLink ? MumbleLink->Context.BuildID : 0, MumbleLink ? MumbleLink->Context.MapID : 0,
				Hex(ExeBase).c_str(), Hex(ExeEnd - ExeBase).c_str(), aPlayer.x, aPlayer.y, aPlayer.z);
			Log(b);
		}

		void RunAsync(std::function<void()> aJob)
		{
			if (Busy) { return; }
			if (Worker.joinable()) { Worker.join(); }
			Busy = true;
			Cancel = false;
			Progress = 0;
			Worker = std::thread([aJob]() { aJob(); Busy = false; });
		}

		// ---------------------------------------------------------------- jobs

		void JobScan(Vec3 aPlayer)
		{
			LogHeader("SCAN", aPlayer);
			SetStatus("Scanning memory...");

			std::vector<Candidate> found;
			ForEachChunk(Progress, Cancel, [&](uintptr_t aBase, const uint8_t* aData, size_t aSize) {
				const float* f = (const float*)aData;
				size_t count = aSize / 4;
				for (size_t i = 0; i + 2 < count; i++)
				{
					float x = f[i];
					for (int si = 0; si < 2; si++)
					{
						float s = SCALES[si];
						if (std::fabs(x - aPlayer.x * s) > TOLERANCE_M * s) { continue; }
						for (int p = 0; p < PATTERNS; p++)
						{
							int enc = si * PATTERNS + p;
							Vec3 v = Decode(f + i, enc);
							if (Length(v - aPlayer) < TOLERANCE_M && found.size() < MAX_CANDIDATES)
							{
								found.push_back({ aBase + i * 4, enc });
							}
						}
					}
				}
			});

			// Per-encoding summary helps tell which representation the game uses.
			size_t perEnc[ENCODINGS] = {};
			for (auto& c : found) { perEnc[c.Enc]++; }
			for (int e = 0; e < ENCODINGS; e++)
			{
				if (perEnc[e]) { Log("  " + EncName(e) + ": " + std::to_string(perEnc[e])); }
			}
			Log("  total candidates: " + std::to_string(found.size()) + (Cancel ? " (cancelled)" : ""));

			std::lock_guard<std::mutex> l(Mutex);
			Candidates = std::move(found);
			Groups.clear();
			FilterRounds = 0;
			Status = "Scan found " + std::to_string(Candidates.size()) + " candidates. Move a few metres, stand still, then Filter.";
		}

		void JobFilter(Vec3 aPlayer)
		{
			std::vector<Candidate> in;
			{ std::lock_guard<std::mutex> l(Mutex); in = Candidates; }

			std::vector<Candidate> out;
			for (size_t i = 0; i < in.size(); i++)
			{
				float f[3];
				if (Read(in[i].Addr, f, sizeof(f)) && Length(Decode(f, in[i].Enc) - aPlayer) < TOLERANCE_M)
				{
					out.push_back(in[i]);
				}
				Progress = (float)(i + 1) / std::max<size_t>(1, in.size());
			}

			LogHeader("FILTER", aPlayer);
			Log("  " + std::to_string(in.size()) + " -> " + std::to_string(out.size()));

			std::lock_guard<std::mutex> l(Mutex);
			Candidates = std::move(out);
			FilterRounds++;
			Status = "Filter kept " + std::to_string(Candidates.size()) + " candidates.";
		}

		void HexDump(uintptr_t aAddr, size_t aSize)
		{
			std::vector<uint8_t> b(aSize);
			if (!Read(aAddr, b.data(), aSize)) { Log("    (unreadable)"); return; }
			for (size_t row = 0; row < aSize; row += 16)
			{
				char line[160];
				int n = std::snprintf(line, sizeof(line), "    +%03zX:", row);
				for (size_t k = 0; k < 16 && row + k < aSize; k++) { n += std::snprintf(line + n, sizeof(line) - n, " %02X", b[row + k]); }
				// Interpret the row as qwords too: pointers into the exe are worth seeing.
				for (size_t k = 0; k + 8 <= 16 && row + k + 8 <= aSize; k += 8)
				{
					uintptr_t q; memcpy(&q, &b[row + k], 8);
					if (InExe(q)) { n += std::snprintf(line + n, sizeof(line) - n, "  [exe+%llX]", (unsigned long long)(q - ExeBase)); }
				}
				Log(line);
			}
		}

		void JobAnalyze(Vec3 aPlayer)
		{
			std::vector<Candidate> cands;
			{ std::lock_guard<std::mutex> l(Mutex); cands = Candidates; }

			LogHeader("ANALYZE", aPlayer);
			SetStatus("Looking for object starts...");

			// 1. For each candidate, walk back up to 0x400 bytes looking for a vtable pointer.
			//    Several vtables may qualify (nested objects); keep them all.
			struct Key { uintptr_t Vtable; int Offset; int Enc; };
			std::vector<Key> keys;
			for (auto& c : cands)
			{
				Log("  candidate " + Hex(c.Addr) + "  " + EncName(c.Enc));
				for (int back = 0; back <= 0x400; back += 8)
				{
					uintptr_t base = (c.Addr & ~(uintptr_t)7) - back;
					uintptr_t q = 0;
					if (!Read(base, q) || !LooksLikeVtable(q)) { continue; }
					int offset = (int)(c.Addr - base);
					bool dup = false;
					for (auto& k : keys) { dup |= (k.Vtable == q && k.Offset == offset && k.Enc == c.Enc); }
					if (!dup && keys.size() < 512) { keys.push_back({ q, offset, c.Enc }); }
					Log("    vtable exe+" + Hex(q - ExeBase) + " at -" + Hex(offset));
				}
			}
			if (keys.empty())
			{
				SetStatus("No object found around the candidates. Stand completely still for 2 seconds and Scan again.");
				return;
			}

			// 2. One pass over memory: find every object starting with one of those vtables.
			SetStatus("Counting objects of " + std::to_string(keys.size()) + " candidate types...");
			std::unordered_map<uintptr_t, std::vector<uintptr_t>> objects; // vtable -> object bases
			for (auto& k : keys) { objects[k.Vtable]; }
			ForEachChunk(Progress, Cancel, [&](uintptr_t aBase, const uint8_t* aData, size_t aSize) {
				const uintptr_t* q = (const uintptr_t*)aData;
				for (size_t i = 0; i < aSize / 8; i++)
				{
					if (!InExe(q[i])) { continue; }
					auto it = objects.find(q[i]);
					if (it != objects.end() && it->second.size() < 20000) { it->second.push_back(aBase + i * 8); }
				}
			});

			// 3. Read each object's position at the candidate offset and see if it is a plausible character.
			std::vector<GroupResult> results;
			for (auto& k : keys)
			{
				GroupResult g;
				g.VtableRva = k.Vtable - ExeBase;
				g.Offset = k.Offset;
				g.Enc = k.Enc;
				auto& objs = objects[k.Vtable];
				g.Objects = objs.size();
				std::vector<float> dists;
				for (uintptr_t o : objs)
				{
					float f[3];
					if (!Read(o + k.Offset, f, sizeof(f))) { continue; }
					Vec3 v = Decode(f, k.Enc);
					if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) { continue; }
					float d = Length(v - aPlayer);
					if (d < 200.0f) { g.Plausible++; dists.push_back(d); }
				}
				std::sort(dists.begin(), dists.end());
				dists.resize(std::min<size_t>(dists.size(), 12));
				g.Nearest = dists;
				results.push_back(g);
			}
			std::sort(results.begin(), results.end(), [](auto& a, auto& b) { return a.Plausible > b.Plausible; });

			for (auto& g : results)
			{
				char b[256];
				std::snprintf(b, sizeof(b), "  TYPE vtable exe+%llX  pos @ +0x%X  %s  objects %zu  within200m %zu  nearest:",
					(unsigned long long)g.VtableRva, g.Offset, EncName(g.Enc).c_str(), g.Objects, g.Plausible);
				std::string line = b;
				for (float d : g.Nearest) { char t[16]; std::snprintf(t, sizeof(t), " %.1f", d); line += t; }
				Log(line);
			}

			// 4. Dump the best few objects around the player's own instance for later field hunting.
			for (size_t i = 0; i < results.size() && i < 3; i++)
			{
				for (auto& c : cands)
				{
					uintptr_t base = c.Addr - results[i].Offset;
					uintptr_t q = 0;
					if (Read(base, q) && q - ExeBase == results[i].VtableRva)
					{
						Log("  DUMP player object " + Hex(base) + " (type exe+" + Hex(results[i].VtableRva) + ")");
						HexDump(base, std::min(0x400, results[i].Offset + 0x40));
						break;
					}
				}
			}

			std::lock_guard<std::mutex> l(Mutex);
			Groups = std::move(results);
			Status = "Analysis done. Hit a few golems so ArcDPS sees them, then press 4.";
		}

		bool LooksLikeHeapPtr(uint64_t q)
		{
			return q >= 0x10000 && q < 0x7FF000000000ull && (q & 7) == 0 && !InExe((uintptr_t)q);
		}

		/// Finds where the in-game agent id (ArcDPS instid) is stored, relative to the position objects.
		void JobFindIds(Vec3 aPlayer)
		{
			// Prefer the character type confirmed in an earlier session for this game build.
			GroupResult best;
			float bestRatio = 0;
			const unsigned build = MumbleLink ? MumbleLink->Context.BuildID : 0;
			for (auto& k : KNOWN_TYPES)
			{
				if (k.Build == build)
				{
					best.VtableRva = k.VtableRva;
					best.Offset = k.Offset;
					best.Enc = k.Enc;
					bestRatio = 1;
				}
			}
			// Otherwise: the type whose objects are mostly near the player (characters around you).
			if (bestRatio == 0)
			{
				std::lock_guard<std::mutex> l(Mutex);
				for (auto& g : Groups)
				{
					float r = g.Objects ? (float)g.Plausible / g.Objects : 0;
					if (g.Plausible >= 3 && r > bestRatio) { bestRatio = r; best = g; }
				}
			}
			if (bestRatio == 0) { SetStatus("No character type known for this game build. Run Scan/Filter/Analyze first."); return; }

			auto agents = ArcAgents::Snapshot();

			LogHeader("FIND IDS", aPlayer);
			char b[256];
			std::snprintf(b, sizeof(b), "  type exe+%llX pos +0x%X %s | arcdps events %llu, agents %zu",
				(unsigned long long)best.VtableRva, best.Offset, EncName(best.Enc).c_str(),
				(unsigned long long)ArcAgents::EventCount(), agents.size());
			Log(b);
			for (auto& [id, a] : agents)
			{
				std::snprintf(b, sizeof(b), "  arc agent instid %u species %u %s%s '%s'", id, a.Species,
					a.IsNpc ? "npc" : "player", a.IsSelf ? " SELF" : "", a.Name.c_str());
				Log(b);
			}
			if (agents.empty())
			{
				SetStatus("ArcDPS hasn't reported any agents. Is ArcDPS loaded? Hit some golems, then retry.");
				return;
			}

			// Enumerate all objects of the type again (addresses may have changed since Analyze).
			SetStatus("Collecting character objects...");
			const uintptr_t vt = best.VtableRva + ExeBase;
			std::vector<uintptr_t> all;
			ForEachChunk(Progress, Cancel, [&](uintptr_t aBase, const uint8_t* aData, size_t aSize) {
				const uintptr_t* q = (const uintptr_t*)aData;
				for (size_t i = 0; i < aSize / 8; i++) { if (q[i] == vt && all.size() < 5000) { all.push_back(aBase + i * 8); } }
			});

			struct Obj { uintptr_t Base; Vec3 Pos; float Dist; };
			std::vector<Obj> objs;
			for (uintptr_t o : all)
			{
				float f[3];
				if (!Read(o + best.Offset, f, sizeof(f))) { continue; }
				Vec3 v = Decode(f, best.Enc);
				float d = Length(v - aPlayer);
				if (std::isfinite(d) && d < 200.0f) { objs.push_back({ o, v, d }); }
			}
			std::sort(objs.begin(), objs.end(), [](auto& a, auto& b) { return a.Dist < b.Dist; });
			for (size_t i = 0; i < objs.size(); i++)
			{
				std::snprintf(b, sizeof(b), "  obj#%zu %s  pos m (%.1f, %.1f, %.1f)  dist %.1f",
					i, Hex(objs[i].Base).c_str(), objs[i].Pos.x, objs[i].Pos.y, objs[i].Pos.z, objs[i].Dist);
				Log(b);
			}

			// Search each object (depth 0) and the objects it points to (depth 1) for known instids.
			// Key: ptrOff (-1 = inside the object itself), fieldOff, width.
			SetStatus("Searching for the agent id field...");
			struct Hit { size_t Obj; uint16_t Inst; };
			std::unordered_map<int64_t, std::vector<Hit>> hits;
			auto keyOf = [](int ptrOff, int fieldOff, int width) {
				return ((int64_t)(ptrOff + 1) << 24) | ((int64_t)fieldOff << 4) | width;
			};
			auto scan = [&](const uint8_t* buf, size_t size, int ptrOff, size_t objIdx) {
				for (size_t f = 0; f + 2 <= size; f += 2)
				{
					uint16_t v16; memcpy(&v16, buf + f, 2);
					if (v16 == 0 || !agents.count(v16)) { continue; }
					int width = 2;
					if ((f & 3) == 0 && f + 4 <= size) { uint32_t v32; memcpy(&v32, buf + f, 4); if (v32 == v16) { width = 4; } }
					auto& v = hits[keyOf(ptrOff, (int)f, width)];
					if (v.empty() || v.back().Obj != objIdx) { v.push_back({ objIdx, v16 }); }
				}
			};

			constexpr size_t OBJ_SPAN = 0x400, CHILD_SPAN = 0x200;
			for (size_t i = 0; i < objs.size(); i++)
			{
				std::vector<uint8_t> ob(OBJ_SPAN);
				size_t span = OBJ_SPAN;
				if (!Read(objs[i].Base, ob.data(), span)) { span = 0x200; if (!Read(objs[i].Base, ob.data(), span)) { continue; } }
				scan(ob.data(), span, -1, i);
				std::vector<uint8_t> cb(CHILD_SPAN);
				for (size_t k = 0; k + 8 <= span; k += 8)
				{
					uint64_t q; memcpy(&q, &ob[k], 8);
					if (LooksLikeHeapPtr(q) && Read((uintptr_t)q, cb.data(), CHILD_SPAN)) { scan(cb.data(), CHILD_SPAN, (int)k, i); }
				}
				Progress = (float)(i + 1) / objs.size();
			}

			// Rank: most distinct objects matched; the player's own object (dist ~0) matching SELF is a strong sign.
			uint16_t selfInst = 0;
			for (auto& [id, a] : agents) { if (a.IsSelf) { selfInst = id; } }
			struct Ranked { int64_t Key; size_t Count; bool SelfOk; };
			std::vector<Ranked> ranked;
			for (auto& [k, v] : hits)
			{
				bool selfOk = false;
				for (auto& h : v) { selfOk |= (objs[h.Obj].Dist < 0.5f && h.Inst == selfInst); }
				ranked.push_back({ k, v.size(), selfOk });
			}
			std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) {
				return a.Count != b.Count ? a.Count > b.Count : a.SelfOk > b.SelfOk;
			});

			auto describe = [](int64_t k) {
				int ptrOff = (int)(k >> 24) - 1, fieldOff = (int)((k >> 4) & 0xFFFFF), width = (int)(k & 0xF);
				char t[96];
				if (ptrOff < 0) { std::snprintf(t, sizeof(t), "[obj+0x%X] u%d", fieldOff, width * 8); }
				else { std::snprintf(t, sizeof(t), "[[obj+0x%X]+0x%X] u%d", ptrOff, fieldOff, width * 8); }
				return std::string(t);
			};

			Log("  --- id field candidates (objects matched / " + std::to_string(objs.size()) + ") ---");
			for (size_t r = 0; r < ranked.size() && r < 20; r++)
			{
				std::string line = "  " + describe(ranked[r].Key) + "  matched " + std::to_string(ranked[r].Count) +
				                   (ranked[r].SelfOk ? "  SELF-OK" : "") + " :";
				for (auto& h : hits[ranked[r].Key])
				{
					auto& a = agents[h.Inst];
					char t[96];
					std::snprintf(t, sizeof(t), " #%zu=%u(%s)", h.Obj, h.Inst, a.IsNpc ? std::to_string(a.Species).c_str() : (a.IsSelf ? "self" : "plr"));
					line += t;
				}
				Log(line);
			}

			std::lock_guard<std::mutex> l(Mutex);
			Status = ranked.empty()
				? "No agent id field found. Send me diagnostic.txt."
				: "Done. Best: " + describe(ranked[0].Key) + " matched " + std::to_string(ranked[0].Count) + ". Send me diagnostic.txt.";
		}

		/// Model/physics object type seen in the first analysis (metres, MumbleLink axes). Build 210545.
		struct VisualType { unsigned Build; uintptr_t VtableRva; int PosOffset; };
		constexpr VisualType VISUAL_TYPES[] = { { 210545, 0x225CD70, 0x2A8 } };

		/// Finds the pointer route between an agent and its model object (whose position moves every
		/// frame, also during dashes where the agent position only updates at the end).
		void JobFindVisual(Vec3 aPlayer)
		{
			const unsigned build = MumbleLink ? MumbleLink->Context.BuildID : 0;
			const GameMemory::Layout* lay = GameMemory::CurrentLayout(build);
			const VisualType* vis = nullptr;
			for (auto& v : VISUAL_TYPES) { if (v.Build == build) { vis = &v; } }
			if (!lay || !vis) { SetStatus("This game build isn't mapped yet."); return; }

			LogHeader("FIND VISUAL", aPlayer);
			SetStatus("Collecting characters and model objects...");
			const uintptr_t charVt = lay->CharVtableRva + ExeBase, visVt = vis->VtableRva + ExeBase;
			std::vector<uintptr_t> chars, visuals;
			ForEachChunk(Progress, Cancel, [&](uintptr_t aBase, const uint8_t* aData, size_t aSize) {
				const uintptr_t* q = (const uintptr_t*)aData;
				for (size_t i = 0; i < aSize / 8; i++)
				{
					if (q[i] == charVt && chars.size() < 5000)   { chars.push_back(aBase + i * 8); }
					if (q[i] == visVt && visuals.size() < 20000) { visuals.push_back(aBase + i * 8); }
				}
			});

			struct VisObj { uintptr_t Base; Vec3 Pos; };
			std::vector<VisObj> vpos;
			for (uintptr_t v : visuals)
			{
				float f[3];
				if (Read(v + vis->PosOffset, f, sizeof(f)) && std::isfinite(f[0])) { vpos.push_back({ v, { f[0], f[1], f[2] } }); }
			}

			// Pair each agent with the model object at the same position (within 1 m).
			struct Pair { uintptr_t Agent, Vis; float Dist; };
			std::vector<Pair> pairs;
			for (uintptr_t o : chars)
			{
				uint64_t agent = 0; float f[3];
				if (!Read(o + lay->AgentPtrOffset, agent) || !LooksLikeHeapPtr(agent)) { continue; }
				if (!Read(o + lay->PosOffset, f, sizeof(f))) { continue; }
				Vec3 p = GameMemory::GameToMumble(f);
				if (Length(p - aPlayer) > 200.0f) { continue; }
				float best = 1e9f; uintptr_t bestV = 0;
				for (auto& v : vpos) { float d = Length(v.Pos - p); if (d < best) { best = d; bestV = v.Base; } }
				if (best < 1.0f) { pairs.push_back({ (uintptr_t)agent, bestV, best }); }
			}
			char b[256];
			std::snprintf(b, sizeof(b), "  characters %zu, model objects %zu, paired %zu", chars.size(), vpos.size(), pairs.size());
			Log(b);
			if (pairs.size() < 3) { SetStatus("Fewer than 3 agent/model pairs found. Stand near more NPCs and retry."); return; }

			// Search pointer routes, up to 3 hops, in both directions. A pointer "hits" if it points
			// into the first 0x400 bytes of the target (the delta is part of the route).
			SetStatus("Searching pointer routes between agents and models...");
			std::unordered_map<std::string, std::vector<size_t>> routes;
			auto hit = [](uint64_t q, uintptr_t target, int& delta) {
				if (q < target || q >= target + 0x400) { return false; }
				delta = (int)(q - target);
				return true;
			};
			auto record = [&](const std::string& route, size_t idx) {
				auto& v = routes[route];
				if (v.empty() || v.back() != idx) { v.push_back(idx); }
			};
			std::vector<uint8_t> b1(0x400), b2(0x200), b3(0x100);
			for (size_t i = 0; i < pairs.size(); i++)
			{
				for (int dir = 0; dir < 2; dir++)
				{
					const uintptr_t from = dir == 0 ? pairs[i].Agent : pairs[i].Vis;
					const uintptr_t to   = dir == 0 ? pairs[i].Vis : pairs[i].Agent;
					const char* tag      = dir == 0 ? "agent" : "model";
					if (!Read(from, b1.data(), b1.size())) { continue; }
					for (size_t o1 = 0; o1 + 8 <= b1.size(); o1 += 8)
					{
						uint64_t q1; memcpy(&q1, &b1[o1], 8);
						int delta;
						char r[128];
						if (hit(q1, to, delta)) { std::snprintf(r, sizeof(r), "%s +0x%zX -> target+0x%X", tag, o1, delta); record(r, i); }
						if (!LooksLikeHeapPtr(q1) || !Read((uintptr_t)q1, b2.data(), b2.size())) { continue; }
						for (size_t o2 = 0; o2 + 8 <= b2.size(); o2 += 8)
						{
							uint64_t q2; memcpy(&q2, &b2[o2], 8);
							if (hit(q2, to, delta)) { std::snprintf(r, sizeof(r), "%s +0x%zX +0x%zX -> target+0x%X", tag, o1, o2, delta); record(r, i); }
							if (!LooksLikeHeapPtr(q2) || !Read((uintptr_t)q2, b3.data(), b3.size())) { continue; }
							for (size_t o3 = 0; o3 + 8 <= b3.size(); o3 += 8)
							{
								uint64_t q3; memcpy(&q3, &b3[o3], 8);
								if (hit(q3, to, delta)) { std::snprintf(r, sizeof(r), "%s +0x%zX +0x%zX +0x%zX -> target+0x%X", tag, o1, o2, o3, delta); record(r, i); }
							}
						}
					}
				}
				Progress = (float)(i + 1) / pairs.size();
			}

			std::vector<std::pair<std::string, size_t>> ranked;
			for (auto& [r, v] : routes) { ranked.push_back({ r, v.size() }); }
			std::sort(ranked.begin(), ranked.end(), [](auto& x, auto& y) { return x.second != y.second ? x.second > y.second : x.first.size() < y.first.size(); });
			Log("  --- routes (pairs matched / " + std::to_string(pairs.size()) + ") ---");
			for (size_t r = 0; r < ranked.size() && r < 25; r++) { Log("  " + ranked[r].first + "  matched " + std::to_string(ranked[r].second)); }

			std::lock_guard<std::mutex> l(Mutex);
			Status = ranked.empty() ? "No route found. Tell me it's done."
			                        : "Done. Best route matched " + std::to_string(ranked[0].second) + "/" + std::to_string(pairs.size()) + ". Tell me it's done.";
		}

		/// Name of the module (DLL/EXE) that contains the address, or "" if none.
		std::string ModuleOf(uintptr_t aAddr)
		{
			HMODULE h = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        (LPCWSTR)aAddr, &h) || !h) { return ""; }
			char path[MAX_PATH] = {};
			GetModuleFileNameA(h, path, MAX_PATH);
			const char* name = strrchr(path, '\\');
			return name ? name + 1 : path;
		}

		std::string RegionInfo(uintptr_t aAddr)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery((LPCVOID)aAddr, &mbi, sizeof(mbi)) != sizeof(mbi)) { return "?"; }
			char t[128];
			std::snprintf(t, sizeof(t), "alloc %s size %s", Hex((uintptr_t)mbi.AllocationBase).c_str(), Hex(mbi.RegionSize).c_str());
			return t;
		}

		/// Hex dump where every qword that points into a module is labelled with that module.
		void DumpAnnotated(uintptr_t aAddr, size_t aSize)
		{
			std::vector<uint8_t> b(aSize);
			if (!Read(aAddr, b.data(), aSize)) { Log("    (unreadable)"); return; }
			for (size_t off = 0; off + 8 <= aSize; off += 8)
			{
				uint64_t q; memcpy(&q, &b[off], 8);
				uint32_t lo = (uint32_t)q, hi = (uint32_t)(q >> 32);
				std::string mod = ModuleOf((uintptr_t)q);
				char line[200];
				std::snprintf(line, sizeof(line), "    %+04d: %016llX  u32 %u %u%s%s", (int)off - 0x40,
					(unsigned long long)q, lo, hi, mod.empty() ? "" : "  -> ", mod.c_str());
				Log(line);
			}
		}

		/// Finds where the NPC species id is stored, using the mapped agent id link and ArcDPS species.
		void JobFindSpecies(Vec3 aPlayer)
		{
			const unsigned build = MumbleLink ? MumbleLink->Context.BuildID : 0;
			const GameMemory::Layout* lay = GameMemory::CurrentLayout(build);
			if (!lay) { SetStatus("This game build isn't mapped yet."); return; }

			auto agents = ArcAgents::Snapshot();
			LogHeader("FIND SPECIES", aPlayer);

			SetStatus("Collecting character objects...");
			const uintptr_t vt = lay->CharVtableRva + ExeBase;
			std::vector<uintptr_t> all;
			ForEachChunk(Progress, Cancel, [&](uintptr_t aBase, const uint8_t* aData, size_t aSize) {
				const uintptr_t* q = (const uintptr_t*)aData;
				for (size_t i = 0; i < aSize / 8; i++) { if (q[i] == vt && all.size() < 5000) { all.push_back(aBase + i * 8); } }
			});

			// Objects whose agent ArcDPS knows as an NPC: we know which species they should have.
			struct Known { uintptr_t Obj, Agent; uint32_t Species; uint16_t Inst; };
			std::vector<Known> known;
			for (uintptr_t o : all)
			{
				uint64_t agent = 0; uint32_t id = 0;
				if (!Read(o + lay->AgentPtrOffset, agent) || !LooksLikeHeapPtr(agent)) { continue; }
				if (!Read((uintptr_t)agent + lay->AgentIdOffset, id) || id > 0xFFFF) { continue; }
				auto it = agents.find((uint16_t)id);
				if (it != agents.end() && it->second.IsNpc && it->second.Species)
				{
					known.push_back({ o, (uintptr_t)agent, it->second.Species, (uint16_t)id });
				}
			}
			char b[256];
			std::snprintf(b, sizeof(b), "  objects %zu, NPCs known to ArcDPS among them %zu", all.size(), known.size());
			Log(b);
			for (auto& k : known)
			{
				std::snprintf(b, sizeof(b), "  npc inst %u species %u '%s' obj %s agent %s", k.Inst, k.Species,
					agents[k.Inst].Name.c_str(), Hex(k.Obj).c_str(), Hex(k.Agent).c_str());
				Log(b);
			}
			if (known.size() < 2) { SetStatus("Need at least 2 NPCs that ArcDPS has seen nearby. Hit more enemies."); return; }

			// Unique agents (the same agent can be referenced by several position objects).
			std::vector<Known> uniq;
			for (auto& k : known)
			{
				bool dup = false;
				for (auto& u : uniq) { dup |= u.Agent == k.Agent; }
				if (!dup) { uniq.push_back(k); }
			}

			// 1. One pass over memory: every place that points to one of these agents ("referrers").
			//    The character object that owns the species is expected to be among them.
			SetStatus("Finding objects that point to the agents...");
			std::unordered_map<uintptr_t, std::vector<uintptr_t>> refs;
			for (auto& u : uniq) { refs[u.Agent].push_back(u.Agent); } // the agent itself as a root too
			ForEachChunk(Progress, Cancel, [&](uintptr_t aBase, const uint8_t* aData, size_t aSize) {
				const uint64_t* q = (const uint64_t*)aData;
				for (size_t i = 0; i < aSize / 8; i++)
				{
					if (!LooksLikeHeapPtr(q[i])) { continue; }
					auto it = refs.find((uintptr_t)q[i]);
					if (it != refs.end() && it->second.size() < 64) { it->second.push_back(aBase + i * 8); }
				}
			});
			for (auto& u : uniq)
			{
				std::snprintf(b, sizeof(b), "  agent %s (species %u): %zu referrers", Hex(u.Agent).c_str(), u.Species, refs[u.Agent].size() - 1);
				Log(b);
			}

			// 2. Around each referrer (window -0x300..+0x100) search depth 0, 1 and 2 for the species.
			//    Key = depth, offset of the 1st pointer (relative to the referrer), 2nd pointer, field.
			SetStatus("Searching around the referrers for the species id...");
			constexpr int BEFORE = 0x300, WIN = 0x400, SPAN1 = 0x200, PTR2 = 0x100, SPAN2 = 0x100;
			auto keyOf = [](int depth, int p1, int p2, int f) {
				return ((int64_t)depth << 56) | ((int64_t)(p1 + 0x1000) << 40) | ((int64_t)p2 << 20) | f;
			};
			std::unordered_map<int64_t, std::vector<size_t>> hits;
			std::unordered_map<int64_t, std::vector<uintptr_t>> examples; // depth-0 referrer addresses
			auto add = [&](int64_t k, size_t idx) {
				auto& h = hits[k];
				if (h.empty() || h.back() != idx) { h.push_back(idx); }
			};
			auto scanU32 = [](const uint8_t* buf, int size, uint32_t want, auto&& onHit) {
				for (int f = 0; f + 4 <= size; f += 4) { uint32_t v; memcpy(&v, buf + f, 4); if (v == want) { onHit(f); } }
			};

			std::vector<uint8_t> w(WIN), c1(SPAN1), c2(SPAN2);
			for (size_t i = 0; i < uniq.size(); i++)
			{
				const uint32_t want = uniq[i].Species;
				for (uintptr_t r : refs[uniq[i].Agent])
				{
					if (Cancel) { break; }
					if (!Read(r - BEFORE, w.data(), WIN)) { continue; }
					// depth 0 stores its (possibly negative) offset in the biased p1 slot
					scanU32(w.data(), WIN, want, [&](int f) {
						int64_t k = keyOf(0, f - BEFORE, 0, 0);
						add(k, i);
						auto& ex = examples[k];
						if (ex.size() < 8) { ex.push_back(r); }
					});
					for (int p1 = 0; p1 + 8 <= WIN; p1 += 8)
					{
						uint64_t q1; memcpy(&q1, &w[p1], 8);
						if (!LooksLikeHeapPtr(q1) || !Read((uintptr_t)q1, c1.data(), SPAN1)) { continue; }
						scanU32(c1.data(), SPAN1, want, [&](int f) { add(keyOf(1, p1 - BEFORE, 0, f), i); });
						for (int p2 = 0; p2 + 8 <= PTR2; p2 += 8)
						{
							uint64_t q2; memcpy(&q2, &c1[p2], 8);
							if (!LooksLikeHeapPtr(q2) || !Read((uintptr_t)q2, c2.data(), SPAN2)) { continue; }
							scanU32(c2.data(), SPAN2, want, [&](int f) { add(keyOf(2, p1 - BEFORE, p2, f), i); });
						}
					}
				}
				Progress = (float)(i + 1) / uniq.size();
			}

			struct Ranked { int64_t Key; size_t Count; size_t Distinct; };
			std::vector<Ranked> ranked;
			for (auto& [k, v] : hits)
			{
				std::vector<uint32_t> sp;
				for (size_t i : v) { if (std::find(sp.begin(), sp.end(), uniq[i].Species) == sp.end()) { sp.push_back(uniq[i].Species); } }
				ranked.push_back({ k, v.size(), sp.size() });
			}
			std::sort(ranked.begin(), ranked.end(), [](auto& x, auto& y) {
				return x.Count != y.Count ? x.Count > y.Count : x.Distinct > y.Distinct;
			});

			auto describe = [](int64_t k) {
				int depth = (int)(k >> 56), p1 = (int)((k >> 40) & 0xFFFF) - 0x1000, p2 = (int)((k >> 20) & 0xFFFFF), f = (int)(k & 0xFFFFF);
				char t[96], a1[16];
				std::snprintf(a1, sizeof(a1), "%s0x%X", p1 < 0 ? "-" : "+", p1 < 0 ? -p1 : p1);
				if (depth == 0)      { std::snprintf(t, sizeof(t), "[ref%s]", a1); }
				else if (depth == 1) { std::snprintf(t, sizeof(t), "[[ref%s]+0x%X]", a1, f); }
				else                 { std::snprintf(t, sizeof(t), "[[[ref%s]+0x%X]+0x%X]", a1, p2, f); }
				return std::string(t);
			};
			Log("  --- species field candidates (NPCs matched / " + std::to_string(uniq.size()) + ", distinct species) ---");
			for (size_t r = 0; r < ranked.size() && r < 25; r++)
			{
				Log("  " + describe(ranked[r].Key) + " u32  matched " + std::to_string(ranked[r].Count) +
				    "  species " + std::to_string(ranked[r].Distinct));
			}

			// Who owns the depth-0 matches? Dump them and name the module each pointer points into
			// (game exe = game data; arcdps.dll or another addon = that addon's own tables).
			int dumped = 0;
			for (size_t r = 0; r < ranked.size() && dumped < 2; r++)
			{
				if ((ranked[r].Key >> 56) != 0 || ranked[r].Count < 3) { continue; }
				dumped++;
				Log("  --- owners of " + describe(ranked[r].Key) + " ---");
				for (uintptr_t ref : examples[ranked[r].Key])
				{
					Log("  referrer " + Hex(ref) + "  region " + RegionInfo(ref));
					DumpAnnotated(ref - 0x40, 0x80);
				}
			}

			std::lock_guard<std::mutex> l(Mutex);
			Status = ranked.empty()
				? "No species field found. Send me diagnostic.txt."
				: "Done. Best: " + describe(ranked[0].Key) + " matched " + std::to_string(ranked[0].Count) + "/" +
				  std::to_string(uniq.size()) + ". Tell me it's done.";
		}
	}

	void Init(const std::string& aLogPath)
	{
		LogPath  = aLogPath;
		Self     = GetCurrentProcess();
		ExeBase  = (uintptr_t)GetModuleHandleW(nullptr);

		auto dos = (IMAGE_DOS_HEADER*)ExeBase;
		auto nt  = (IMAGE_NT_HEADERS64*)(ExeBase + dos->e_lfanew);
		ExeEnd   = ExeBase + nt->OptionalHeader.SizeOfImage;
		ExeCodeEnd = ExeBase + nt->OptionalHeader.BaseOfCode + nt->OptionalHeader.SizeOfCode;
	}

	void Shutdown()
	{
		Cancel = true;
		if (Worker.joinable()) { Worker.join(); }
	}

	void RenderWindow(bool* aOpen, const Vec3& aPlayer)
	{
		if (!*aOpen) { return; }
		ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Ender's Echo - Memory diagnostic", aOpen))
		{
			ImGui::End();
			return;
		}

		ImGui::TextWrapped("Use in open world or the Special Forces Training Area with NPCs nearby. "
		                   "Stand still for each step. Don't use a mount.");
		ImGui::Text("Player (m): %.2f  %.2f  %.2f", aPlayer.x, aPlayer.y, aPlayer.z);
		ImGui::Separator();

		bool busy = Busy;
		size_t candCount;
		int rounds;
		{
			std::lock_guard<std::mutex> l(Mutex);
			candCount = Candidates.size();
			rounds = FilterRounds;
		}

		if (busy) { ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f); }
		if (ImGui::Button("1. Scan") && !busy)    { RunAsync([aPlayer]() { JobScan(aPlayer); }); }
		ImGui::SameLine();
		if (ImGui::Button("2. Filter") && !busy && candCount) { RunAsync([aPlayer]() { JobFilter(aPlayer); }); }
		ImGui::SameLine();
		if (ImGui::Button("3. Analyze") && !busy && candCount && candCount <= 500) { RunAsync([aPlayer]() { JobAnalyze(aPlayer); }); }
		ImGui::SameLine();
		bool haveGroups;
		{ std::lock_guard<std::mutex> l(Mutex); haveGroups = !Groups.empty(); }
		bool knownBuild = false;
		for (auto& k : KNOWN_TYPES) { knownBuild |= MumbleLink && k.Build == MumbleLink->Context.BuildID; }
		if (ImGui::Button("4. Find agent IDs") && !busy && (haveGroups || knownBuild)) { RunAsync([aPlayer]() { JobFindIds(aPlayer); }); }
		ImGui::SameLine();
		if (ImGui::Button("5. Find species") && !busy && knownBuild) { RunAsync([aPlayer]() { JobFindSpecies(aPlayer); }); }
		ImGui::SameLine();
		if (ImGui::Button("6. Find model link") && !busy && knownBuild) { RunAsync([aPlayer]() { JobFindVisual(aPlayer); }); }
		if (knownBuild) { ImGui::TextDisabled("This game build is already mapped: steps 4-6 work without steps 1-3."); }
		if (busy) { ImGui::PopStyleVar(); }

		size_t arcNpcs = 0;
		auto arcSnap = ArcAgents::Snapshot();
		for (auto& [id, a] : arcSnap) { arcNpcs += a.IsNpc; }
		ImGui::Text("ArcDPS: %llu events, %zu agents seen (%zu NPCs)", (unsigned long long)ArcAgents::EventCount(), arcSnap.size(), arcNpcs);
		if (busy)
		{
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) { Cancel = true; }
			ImGui::ProgressBar(Progress, ImVec2(-1, 0));
		}

		{
			std::lock_guard<std::mutex> l(Mutex);
			ImGui::TextWrapped("%s", Status.c_str());
		}
		ImGui::Text("Candidates: %zu   Filter rounds: %d", candCount, rounds);
		if (candCount > 500) { ImGui::TextDisabled("Filter until 500 or fewer remain (move between rounds), then Analyze."); }
		else if (candCount && rounds < 2) { ImGui::TextDisabled("Do at least two filter rounds, moving between them."); }

		std::lock_guard<std::mutex> l(Mutex);
		if (!Groups.empty() && ImGui::BeginTable("groups", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
		{
			ImGui::TableSetupColumn("vtable");
			ImGui::TableSetupColumn("pos offset");
			ImGui::TableSetupColumn("encoding");
			ImGui::TableSetupColumn("objects");
			ImGui::TableSetupColumn("< 200 m");
			ImGui::TableHeadersRow();
			for (auto& g : Groups)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::Text("exe+%llX", (unsigned long long)g.VtableRva);
				ImGui::TableNextColumn(); ImGui::Text("+0x%X", g.Offset);
				ImGui::TableNextColumn(); ImGui::Text("%s", EncName(g.Enc).c_str());
				ImGui::TableNextColumn(); ImGui::Text("%zu", g.Objects);
				ImGui::TableNextColumn(); ImGui::Text("%zu", g.Plausible);
			}
			ImGui::EndTable();
		}
		ImGui::End();
	}
}
