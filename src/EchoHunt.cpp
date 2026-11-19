#include "EchoHunt.h"

#include "Shared.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

namespace
{
	constexpr float  HORIZONTAL_M = 70.0f;  // arena radius is ~45 m
	constexpr float  VERTICAL_M   = 5.0f;
	constexpr size_t MAX_CANDS    = 4000000;

	// 0 = game units: inches, stored (x, z, -y).   1 = metres in MumbleLink axes (x, y, z).
	Vec3 Decode(const float* f, uint8_t aEnc)
	{
		return aEnc == 0 ? GameMemory::GameToMumble(f) : Vec3{ f[0], f[1], f[2] };
	}

	double NowSeconds()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::string Hex(uintptr_t v) { char b[32]; std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v); return b; }

	Vec3 PlayerPos()
	{
		return MumbleLink ? Vec3{ MumbleLink->AvatarPosition.X, MumbleLink->AvatarPosition.Y, MumbleLink->AvatarPosition.Z } : Vec3{};
	}
}

EchoHunt::~EchoHunt() { Stop(); }

void EchoHunt::Log(const std::string& aLine)
{
	std::ofstream f(mLogPath, std::ios::app);
	f << aLine << '\n';
}

void EchoHunt::Start(const std::string& aLogPath, const GameMemory::Layout& aLayout)
{
	Stop();
	mLogPath = aLogPath;
	mLayout = aLayout;
	mSampleTicks = 0;
	{ std::lock_guard<std::mutex> l(mMutex); mCands.clear(); mChars.clear(); }
	char t[64];
	std::time_t now = std::time(nullptr);
	std::strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
	Log(std::string("\n=== ECHO HUNT v3 ") + t);
	mStage = Stage::Armed;
}

void EchoHunt::Stop()
{
	mCancel = true;
	if (mWorker.joinable()) { mWorker.join(); }
	mCancel = false;
	if (mStage == Stage::Sampling) { LogSurvivors("stopped, sampling result"); }
	mStage = Stage::Off;
}

std::string EchoHunt::Status() const
{
	size_t n;
	{ std::lock_guard<std::mutex> l(mMutex); n = mCands.size(); }
	switch (mStage.load())
	{
		case Stage::Armed:    return "armed: starts when you enter combat in Hall of Chains";
		case Stage::Scanning: return "scanning memory (keep fighting)...";
		case Stage::Motion:   return "looking for the running Echo among " + std::to_string(n) + " candidates...";
		case Stage::Routes:   return "searching routes...";
		case Stage::Sampling: return "done, " + std::to_string(n) + " moving values. Tell me it's done.";
		case Stage::Done:     return "finished without a result. Tell me it's done.";
		default:              return "off";
	}
}

void EchoHunt::Feed(const Vec3& aPlayer, bool aInCombat, unsigned aMapId, double aTime)
{
	if (mStage == Stage::Armed && aInCombat && aMapId == MAP_HALL_OF_CHAINS)
	{
		mStage = Stage::Scanning;
		if (mWorker.joinable()) { mWorker.join(); }
		const Vec3 c = aPlayer;
		mWorker = std::thread([this, c]() { Run(c); });
	}

	// After the route search: keep counting how often each survivor moves at running speed.
	if (mStage == Stage::Sampling && aTime - mSampleLast >= 0.1)
	{
		mSampleLast = aTime;
		SampleCandidates();
		mSampleTicks++;
	}
}

void EchoHunt::Run(Vec3 aCenter)
{
	Log("  combat started, scanning around (" + std::to_string((int)aCenter.x) + ", " + std::to_string((int)aCenter.z) + ")");
	Scan(aCenter);
	if (mCancel) { return; }

	mStage = Stage::Motion;
	if (!MotionFilter() || mCancel) { mStage = Stage::Done; return; }
	LogSurvivors("moving at running speed");

	mStage = Stage::Routes;
	RouteSearch();
	mStage = Stage::Sampling;
}

void EchoHunt::Scan(Vec3 c)
{
	const double t0 = NowSeconds();
	const float in = 1.0f / INCH_TO_METRE;
	const uint64_t charVt = GameMemory::ExeBase() + mLayout.CharVtableRva;
	std::vector<Cand> found;
	std::vector<uintptr_t> chars;
	std::vector<uint8_t> buf(1 << 20);

	for (auto& r : GameMemory::ScannableRegions())
	{
		for (size_t off = 0; off < r.Size; off += buf.size())
		{
			if (mCancel) { return; }
			size_t n = std::min(buf.size(), r.Size - off);
			if (!GameMemory::Read(r.Base + off, buf.data(), n)) { continue; }

			const uint64_t* q = (const uint64_t*)buf.data();
			for (size_t i = 0; i < n / 8; i++) { if (q[i] == charVt && chars.size() < 5000) { chars.push_back(r.Base + off + i * 8); } }

			const float* f = (const float*)buf.data();
			for (size_t i = 0; i + 2 < n / 4 && found.size() < MAX_CANDS; i++)
			{
				const float x = f[i];
				if (std::fabs(x - c.x * in) < HORIZONTAL_M * in &&
				    std::fabs(-f[i + 2] - c.y * in) < VERTICAL_M * in &&
				    std::fabs(f[i + 1] - c.z * in) < HORIZONTAL_M * in)
				{
					found.push_back({ r.Base + off + i * 4, 0, Decode(f + i, 0) });
				}
				else if (std::fabs(x - c.x) < HORIZONTAL_M &&
				         std::fabs(f[i + 1] - c.y) < VERTICAL_M &&
				         std::fabs(f[i + 2] - c.z) < HORIZONTAL_M)
				{
					found.push_back({ r.Base + off + i * 4, 1, Decode(f + i, 1) });
				}
			}
		}
	}

	char b[128];
	std::snprintf(b, sizeof(b), "  scan: %zu candidates, %zu characters, %.1f s", found.size(), chars.size(), NowSeconds() - t0);
	Log(b);
	std::lock_guard<std::mutex> l(mMutex);
	mCands = std::move(found);
	mChars = std::move(chars);
}

void EchoHunt::ReadAll(const std::vector<Cand>& aIn, std::vector<Vec3>& aPos, std::vector<double>& aTime, std::vector<char>& aOk)
{
	aPos.assign(aIn.size(), Vec3{});
	aTime.assign(aIn.size(), 0);
	aOk.assign(aIn.size(), 0);
	std::vector<uint8_t> buf(0x10000 + 16);
	size_t i = 0;
	while (i < aIn.size())
	{
		const uintptr_t start = aIn[i].Addr;
		size_t j = i;
		while (j < aIn.size() && aIn[j].Addr + 12 - start <= 0x10000) { j++; }
		const size_t span = aIn[j - 1].Addr + 12 - start;
		const bool ok = GameMemory::Read(start, buf.data(), span);
		const double t = NowSeconds();
		for (size_t k = i; k < j; k++)
		{
			float f[3];
			if (ok) { memcpy(f, buf.data() + (aIn[k].Addr - start), 12); }
			else if (!GameMemory::Read(aIn[k].Addr, f, 12)) { continue; }
			aPos[k] = Decode(f, aIn[k].Enc);
			aTime[k] = t;
			aOk[k] = 1;
		}
		i = j;
	}
}

bool EchoHunt::MotionFilter()
{
	std::vector<Cand> cands;
	{ std::lock_guard<std::mutex> l(mMutex); cands = mCands; }

	auto speedOk = [](const Vec3& a, const Vec3& b, double dt, Vec3& dir) {
		if (dt < 0.05) { return false; }
		Vec3 d = Flat(b - a);
		float v = Length(d) / (float)dt;
		dir = Normalize(d);
		return v > 7.0f && v < 14.0f && std::fabs(b.y - a.y) < 1.5f;
	};

	for (int attempt = 0; attempt < 40 && !mCancel; attempt++)
	{
		std::vector<Vec3> p1, p2, p3; std::vector<double> t1, t2, t3; std::vector<char> ok1, ok2, ok3;
		ReadAll(cands, p1, t1, ok1);
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		ReadAll(cands, p2, t2, ok2);
		const Vec3 pl = PlayerPos();

		std::vector<Cand> moving;
		std::vector<Vec3> dirs;
		std::vector<double> times;
		for (size_t i = 0; i < cands.size(); i++)
		{
			Vec3 dir;
			if (!ok1[i] || !ok2[i] || FlatDistance(p2[i], pl) < 3.0f) { continue; }
			if (!speedOk(p1[i], p2[i], t2[i] - t1[i], dir)) { continue; }
			Cand c = cands[i];
			c.Stored = p2[i];
			moving.push_back(c);
			dirs.push_back(dir);
			times.push_back(t2[i]);
		}
		if (moving.empty())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			continue;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		ReadAll(moving, p3, t3, ok3);
		std::vector<Cand> steady;
		for (size_t i = 0; i < moving.size(); i++)
		{
			Vec3 dir;
			if (!ok3[i] || !speedOk(moving[i].Stored, p3[i], t3[i] - times[i], dir)) { continue; }
			if (Dot(dir, dirs[i]) < 0.96f) { continue; }
			Cand c = moving[i];
			c.Stored = p3[i];
			steady.push_back(c);
		}
		Log("  motion attempt " + std::to_string(attempt) + ": " + std::to_string(moving.size()) + " moving, " +
		    std::to_string(steady.size()) + " steady");
		if (!steady.empty())
		{
			std::lock_guard<std::mutex> l(mMutex);
			mCands = std::move(steady);
			return true;
		}
	}
	Log("  motion filter: nothing moved like the Echo");
	return false;
}

void EchoHunt::RouteSearch()
{
	std::vector<Cand> cands;
	std::vector<uintptr_t> chars;
	{ std::lock_guard<std::mutex> l(mMutex); cands = mCands; chars = mChars; }

	const Vec3 pl = PlayerPos();
	Log("  --- route search from " + std::to_string(chars.size()) + " characters ---");

	std::vector<uint8_t> b1(0x400), b2(0x200), b3(0x100);
	struct Ptr { uint64_t Value; int O1, O2, O3; };
	size_t echoLike = 0;

	for (uintptr_t obj : chars)
	{
		if (mCancel) { return; }
		uint64_t agent = 0; uint32_t id = 0; float raw[3];
		if (!GameMemory::Read(obj + mLayout.AgentPtrOffset, agent) || !GameMemory::LooksLikeHeapPtr(agent)) { continue; }
		GameMemory::Read((uintptr_t)agent + mLayout.AgentIdOffset, id);
		if (!GameMemory::Read(obj + mLayout.PosOffset, raw, sizeof(raw))) { continue; }
		const Vec3 pos = GameMemory::GameToMumble(raw);
		if (FlatDistance(pos, pl) > 150.0f) { continue; }

		// All pointers reachable from this agent within 3 hops.
		std::vector<Ptr> ptrs;
		if (!GameMemory::Read((uintptr_t)agent, b1.data(), b1.size())) { continue; }
		for (int o1 = 0; o1 + 8 <= (int)b1.size(); o1 += 8)
		{
			uint64_t q1; memcpy(&q1, &b1[o1], 8);
			if (!GameMemory::LooksLikeHeapPtr(q1)) { continue; }
			ptrs.push_back({ q1, o1, -1, -1 });
			if (!GameMemory::Read((uintptr_t)q1, b2.data(), b2.size())) { continue; }
			for (int o2 = 0; o2 + 8 <= (int)b2.size(); o2 += 8)
			{
				uint64_t q2; memcpy(&q2, &b2[o2], 8);
				if (!GameMemory::LooksLikeHeapPtr(q2)) { continue; }
				ptrs.push_back({ q2, o1, o2, -1 });
				if (!GameMemory::Read((uintptr_t)q2, b3.data(), b3.size())) { continue; }
				for (int o3 = 0; o3 + 8 <= (int)b3.size(); o3 += 8)
				{
					uint64_t q3; memcpy(&q3, &b3[o3], 8);
					if (GameMemory::LooksLikeHeapPtr(q3)) { ptrs.push_back({ q3, o1, o2, o3 }); }
				}
			}
		}
		std::sort(ptrs.begin(), ptrs.end(), [](auto& a, auto& b) { return a.Value < b.Value; });

		// Routes ending at most 0x800 bytes before a moving candidate.
		std::vector<std::string> lines;
		size_t reached = 0;
		for (auto& c : cands)
		{
			auto lo = std::lower_bound(ptrs.begin(), ptrs.end(), (uint64_t)(c.Addr - 0x800),
			                           [](const Ptr& p, uint64_t v) { return p.Value < v; });
			bool any = false;
			for (auto it = lo; it != ptrs.end() && it->Value <= c.Addr; ++it)
			{
				any = true;
				if (lines.size() >= 40) { continue; }
				char r[160];
				const int d = (int)(c.Addr - it->Value);
				if (it->O2 < 0)      { std::snprintf(r, sizeof(r), "      [agent+0x%X] + 0x%X  -> %s", it->O1, d, Hex(c.Addr).c_str()); }
				else if (it->O3 < 0) { std::snprintf(r, sizeof(r), "      [[agent+0x%X]+0x%X] + 0x%X  -> %s", it->O1, it->O2, d, Hex(c.Addr).c_str()); }
				else                 { std::snprintf(r, sizeof(r), "      [[[agent+0x%X]+0x%X]+0x%X] + 0x%X  -> %s", it->O1, it->O2, it->O3, d, Hex(c.Addr).c_str()); }
				lines.push_back(r);
			}
			reached += any;
		}
		if (reached == 0) { continue; }

		echoLike++;
		char h[200];
		std::snprintf(h, sizeof(h), "  character agent id %u at (%.1f, %.1f), %.1f m from player: reaches %zu of %zu moving values",
			id, pos.x, pos.z, FlatDistance(pos, pl), reached, cands.size());
		Log(h);
		for (auto& l : lines) { Log(l); }
	}
	Log("  routes done: " + std::to_string(echoLike) + " characters reach the moving values");
}

void EchoHunt::SampleCandidates()
{
	std::lock_guard<std::mutex> l(mMutex);
	if (mCands.size() > 20000) { return; }
	for (auto& c : mCands)
	{
		float f[3];
		if (!GameMemory::Read(c.Addr, f, sizeof(f))) { continue; }
		Vec3 p = Decode(f, c.Enc);
		if (c.HaveLast)
		{
			float step = FlatDistance(p, c.Last);
			if (step > 0.3f && step < 2.5f) { c.Steps++; }
		}
		c.Samples++;
		c.Last = p;
		c.HaveLast = true;
	}
}

void EchoHunt::LogSurvivors(const char* aTitle)
{
	std::lock_guard<std::mutex> l(mMutex);
	std::vector<size_t> order(mCands.size());
	for (size_t i = 0; i < order.size(); i++) { order[i] = i; }
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return mCands[a].Steps > mCands[b].Steps; });
	Log(std::string("  --- ") + aTitle + ": " + std::to_string(mCands.size()) + " ---");
	for (size_t oi = 0; oi < order.size() && oi < 150; oi++)
	{
		const Cand& c = mCands[order[oi]];
		char b[200];
		std::snprintf(b, sizeof(b), "  value %s %s at (%.1f, %.1f)  moving %d/%d", Hex(c.Addr).c_str(),
			c.Enc == 0 ? "inches" : "metres", c.Stored.x, c.Stored.z, c.Steps, c.Samples);
		std::string line = b;
		for (int back = 0; back <= 0x800; back += 8)
		{
			uintptr_t base = (c.Addr & ~(uintptr_t)7) - back;
			uint64_t q = 0;
			if (GameMemory::Read(base, q) && GameMemory::InExe((uintptr_t)q))
			{
				std::snprintf(b, sizeof(b), "  owner exe+0x%llX at -0x%X", (unsigned long long)(q - GameMemory::ExeBase()), (int)(c.Addr - base));
				line += b;
				break;
			}
		}
		Log(line);
	}
}
