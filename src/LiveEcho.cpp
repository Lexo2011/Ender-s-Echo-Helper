#include "LiveEcho.h"

#include "ArcAgents.h"
#include "Shared.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace
{
	constexpr float ECHO_SPEED = 10.16f;          // 400 in/s
	constexpr float ECHO_SPEED_TOLERANCE = 0.7f;
	// Measured: real Echo dashes 10.1-10.7 m/s; wrong picks (adds, spiders, skeletons) 8.8-9.7 m/s.
	constexpr float ECHO_SPEED_MIN = 9.95f;
	constexpr float ECHO_SPEED_MAX = 11.0f;
	constexpr size_t MIN_ECHO_VALUES = 8;

	double Now()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	Vec3 PlayerPos()
	{
		return MumbleLink ? Vec3{ MumbleLink->AvatarPosition.X, MumbleLink->AvatarPosition.Y, MumbleLink->AvatarPosition.Z } : Vec3{};
	}

	// 0 = game units: inches, stored (x, z, -y).   1 = metres in MumbleLink axes (x, y, z).
	Vec3 Decode(const float* f, uint8_t aEnc)
	{
		return aEnc == 0 ? GameMemory::GameToMumble(f) : Vec3{ f[0], f[1], f[2] };
	}

	bool ReadCand(uintptr_t aAddr, uint8_t aEnc, Vec3& aOut)
	{
		float f[3];
		if (!GameMemory::Read(aAddr, f, sizeof(f))) { return false; }
		aOut = Decode(f, aEnc);
		return std::isfinite(aOut.x) && std::isfinite(aOut.y) && std::isfinite(aOut.z);
	}
}

LiveEcho::LiveEcho(const GameMemory::Layout& aLayout)
	: mLayout(aLayout), mVtable(GameMemory::ExeBase() + aLayout.CharVtableRva)
{
	mWorker = std::thread([this]() { WorkerLoop(); });
}

LiveEcho::~LiveEcho()
{
	mStop = true;
	if (mWorker.joinable()) { mWorker.join(); }
	if (mScanJob.valid()) { mScanJob.wait(); }
}

// ------------------------------------------------------------------------------------ Dhuum mode

void LiveEcho::ResetFight()
{
	{ std::lock_guard<std::mutex> l(mClusterMutex); mCluster.clear(); }
	mCands.clear();
	mStage = 0;
	mLost = false;
	mFrozen = false;
	mLastScan = 0;
	mFrozenSince = 0;
	mStanding.clear();
	if (mScanJob.valid()) { mScanJob.wait(); mScanJob = {}; }
}

/// Every position-like value within 70 m of the centre at floor height, using all CPU cores.
/// Every position-like value within 70 m of the centre at floor height, using all CPU cores.
std::vector<LiveEcho::Cand> LiveEcho::DoScan(const Vec3& c, double& aSeconds)
{
	const double t0 = Now();
	const auto regions = GameMemory::ScannableRegions();
	const unsigned threads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));

	std::atomic<size_t> next{ 0 };
	std::vector<std::vector<Cand>> parts(threads);
	std::vector<std::thread> pool;
	for (unsigned ti = 0; ti < threads; ti++)
	{
		pool.emplace_back([&, ti]() {
			const float in = 1.0f / INCH_TO_METRE;
			const float H = 70.0f, V = 5.0f;
			std::vector<uint8_t> buf(1 << 20);
			auto& out = parts[ti];
			for (size_t ri; (ri = next++) < regions.size();)
			{
				const auto& r = regions[ri];
				for (size_t off = 0; off < r.Size; off += buf.size())
				{
					if (mStop || !mInFight) { return; }
					const size_t n = std::min(buf.size(), r.Size - off);
					if (!GameMemory::Read(r.Base + off, buf.data(), n)) { continue; }
					const float* f = (const float*)buf.data();
					for (size_t i = 0; i + 2 < n / 4; i++)
					{
						const float x = f[i];
						if (std::fabs(x - c.x * in) < H * in && std::fabs(-f[i + 2] - c.y * in) < V * in && std::fabs(f[i + 1] - c.z * in) < H * in)
						{
							out.push_back({ r.Base + off + i * 4, 0 });
						}
						else if (std::fabs(x - c.x) < H && std::fabs(f[i + 1] - c.y) < V && std::fabs(f[i + 2] - c.z) < H)
						{
							out.push_back({ r.Base + off + i * 4, 1 });
						}
					}
				}
			}
		});
	}
	for (auto& t : pool) { t.join(); }

	std::vector<Cand> all;
	if (mStop || !mInFight) { return all; }
	for (auto& p : parts) { all.insert(all.end(), p.begin(), p.end()); }
	std::sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) { return a.Addr < b.Addr; });
	aSeconds = Now() - t0;
	return all;
}

void LiveEcho::ScanArena(const Vec3& c, bool aKeepTracking)
{
	if (!aKeepTracking) { mStage = 1; }
	double took = 0;
	std::vector<Cand> found = DoScan(c, took);
	mLastScan = Now();
	if (mStop || !mInFight) { if (!aKeepTracking) { mStage = 0; } return; }
	mCands = std::move(found);
	if (!aKeepTracking) { mStage = 2; }

	char b[160];
	std::snprintf(b, sizeof(b), "Arena scan: %zu values in %.1f s, %.1f s after the pull.", mCands.size(), took, Now() - mFightStart);
	APIDefs->Log(LOGL_INFO, ADDON_NAME, b);
}

/// Reads all candidates block-wise (they must be sorted by address).
void LiveEcho::ReadAll(const std::vector<Cand>& in, std::vector<Vec3>& pos, std::vector<double>& tm, std::vector<char>& ok)
{
	pos.assign(in.size(), Vec3{}); tm.assign(in.size(), 0); ok.assign(in.size(), 0);
	std::vector<uint8_t> buf(0x10000 + 16);
	size_t i = 0;
	while (i < in.size())
	{
		const uintptr_t start = in[i].Addr;
		size_t j = i;
		while (j < in.size() && in[j].Addr + 12 - start <= 0x10000) { j++; }
		const bool got = GameMemory::Read(start, buf.data(), in[j - 1].Addr + 12 - start);
		const double t = Now();
		for (size_t k = i; k < j; k++)
		{
			float f[3];
			if (got) { memcpy(f, buf.data() + (in[k].Addr - start), 12); }
			else if (!GameMemory::Read(in[k].Addr, f, 12)) { continue; }
			pos[k] = Decode(f, in[k].Enc); tm[k] = t; ok[k] = 1;
		}
		i = j;
	}
}

/// Picks the values that currently sit where the Echo stands.
void LiveEcho::BuildStanding(const Vec3& aAnchor)
{
	std::vector<Vec3> p; std::vector<double> t; std::vector<char> ok;
	ReadAll(mCands, p, t, ok);
	mStanding.clear(); mStandRef.clear();
	for (size_t i = 0; i < mCands.size(); i++)
	{
		if (!ok[i] || FlatDistance(p[i], aAnchor) > 1.5f || std::fabs(p[i].y - aAnchor.y) > 2.5f) { continue; }
		mStanding.push_back(mCands[i]);
		mStandRef.push_back(p[i]);
		if (mStanding.size() >= 4000) { break; }
	}
	mStandPrev = mStandRef;
	mStandPrevT.assign(mStanding.size(), Now());
	mStandingAt = aAnchor;
}

/// Fast check of the standing values: has a group of them started dashing (Echo speed, one
/// direction, away from the wall)? Then follow them.
bool LiveEcho::WatchStanding()
{
	if (mStanding.empty()) { return false; }
	std::vector<Vec3> p; std::vector<double> t; std::vector<char> ok;
	ReadAll(mStanding, p, t, ok);

	std::vector<size_t> movers;
	Vec3 dirSum;
	for (size_t i = 0; i < mStanding.size(); i++)
	{
		if (!ok[i]) { continue; }
		const double dt = t[i] - mStandPrevT[i];
		const float away = FlatDistance(p[i], mStandRef[i]);
		const float v = dt > 0.02 ? FlatDistance(p[i], mStandPrev[i]) / (float)dt : 0.0f;
		mStandPrev[i] = p[i];
		mStandPrevT[i] = t[i];
		if (away < 0.6f || away > 6.0f || v < 8.0f || v > 13.0f || std::fabs(p[i].y - mStandRef[i].y) > 1.5f) { continue; }
		movers.push_back(i);
		dirSum = dirSum + Normalize(Flat(p[i] - mStandRef[i]));
	}
	if (movers.size() < 3) { return false; }

	// One direction: keep the movers that agree with the common direction.
	const Vec3 dir = Normalize(dirSum);
	std::vector<Cand> group;
	for (size_t i : movers)
	{
		if (Dot(Normalize(Flat(p[i] - mStandRef[i])), dir) > 0.95f) { group.push_back(mStanding[i]); }
	}
	if (group.size() < 3) { return false; }
	mLockedSize = group.size();
	if (group.size() > 48) { group.resize(48); }

	{ std::lock_guard<std::mutex> l(mClusterMutex); mCluster = std::move(group); }
	mLost = false;
	mFrozen = false;
	char b[128];
	std::snprintf(b, sizeof(b), "Next dash picked up from %zu standing values (%zu moving).", mStanding.size(), movers.size());
	APIDefs->Log(LOGL_INFO, ADDON_NAME, b);
	mStanding.clear();
	return true;
}

bool LiveEcho::FindEchoCluster(const Vec3& aPlayer, bool aHaveAnchor, const Vec3& aAnchor, size_t aMinSize, bool aAwayFromAnchor)
{
	auto readAll = [](const std::vector<Cand>& in, std::vector<Vec3>& pos, std::vector<double>& tm, std::vector<char>& ok) {
		ReadAll(in, pos, tm, ok);
	};

	std::vector<Vec3> p1, p2, p3; std::vector<double> t1, t2, t3; std::vector<char> o1, o2, o3;
	readAll(mCands, p1, t1, o1);
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	readAll(mCands, p2, t2, o2);

	const Vec3 camera = MumbleLink ? Vec3{ MumbleLink->CameraPosition.X, MumbleLink->CameraPosition.Y, MumbleLink->CameraPosition.Z } : Vec3{ 1e9f, 0, 1e9f };
	struct Mover { Cand C; Vec3 P0; Vec3 P; Vec3 Dir; double T; float V; };
	std::vector<Mover> moving;
	for (size_t i = 0; i < mCands.size(); i++)
	{
		if (!o1[i] || !o2[i]) { continue; }
		const double dt = t2[i] - t1[i];
		const Vec3 d = Flat(p2[i] - p1[i]);
		const float v = dt > 0.05 ? Length(d) / (float)dt : 0.0f;
		if (v < 7.0f || v > 14.0f || std::fabs(p2[i].y - p1[i].y) > 1.5f) { continue; }
		const float dPlayer = FlatDistance(p2[i], aPlayer);
		if (dPlayer < 3.0f || dPlayer > 75.0f) { continue; } // inside the arena, not you
		if (FlatDistance(p2[i], camera) < 3.0f) { continue; } // the game camera follows you
		moving.push_back({ mCands[i], p1[i], p2[i], Normalize(d), t2[i], v });
	}
	if (moving.empty()) { return false; }

	// Confirm: same speed range and direction a moment later.
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	std::vector<Cand> mc;
	for (auto& m : moving) { mc.push_back(m.C); }
	readAll(mc, p3, t3, o3);
	std::vector<Mover> steady;
	for (size_t i = 0; i < moving.size(); i++)
	{
		if (!o3[i]) { continue; }
		const double dt = t3[i] - moving[i].T;
		const Vec3 d = Flat(p3[i] - moving[i].P);
		const float v = dt > 0.05 ? Length(d) / (float)dt : 0.0f;
		if (v < 7.0f || v > 14.0f || Dot(Normalize(d), moving[i].Dir) < 0.96f) { continue; }
		Mover m = moving[i];
		m.P = p3[i];
		m.V = 0.5f * (m.V + v);
		steady.push_back(m);
	}
	if (steady.empty()) { return false; }

	// Group into clusters (one per moving thing) and pick the one closest to the Echo's speed.
	struct Cluster
	{
		Vec3 Sum, OriginSum, DirSum; float VSum = 0; std::vector<Cand> Members;
		Vec3 Center() const { return Sum * (1.0f / Members.size()); }
		Vec3 Origin() const { return OriginSum * (1.0f / Members.size()); }
		Vec3 Dir() const { return Normalize(DirSum); }
		float Speed() const { return VSum / Members.size(); }
	};
	std::vector<Cluster> clusters;
	for (auto& m : steady)
	{
		Cluster* hit = nullptr;
		for (auto& cl : clusters) { if (FlatDistance(cl.Center(), m.P) < 2.5f) { hit = &cl; break; } }
		if (!hit) { clusters.push_back({}); hit = &clusters.back(); }
		hit->Sum = hit->Sum + m.P;
		hit->OriginSum = hit->OriginSum + m.P0;
		hit->DirSum = hit->DirSum + m.Dir;
		hit->VSum += m.V;
		hit->Members.push_back(m.C);
	}

	// How directly a cluster runs at the player (the Echo always dashes at a player).
	auto towardPlayer = [&](const Cluster& cl) { return Dot(cl.Dir(), Normalize(Flat(aPlayer - cl.Center()))); };

	// Only dashes at the Echo's speed. While tracking: only one that starts where the Echo stands
	// (the dash began < 0.5 s ago, so its origin is within a few metres of the wall).
	// First dash: it starts at the arena edge, so nothing close to the player (adds attacking you).
	std::vector<Cluster> ok;
	for (auto& cl : clusters)
	{
		if (cl.Speed() < ECHO_SPEED_MIN || cl.Speed() > ECHO_SPEED_MAX) { continue; }
		// The Echo's dash shows up as many values moving together (dozens to hundreds);
		// messengers, adds and stray values only as a handful.
		if (cl.Members.size() < aMinSize) { continue; }
		// Correction: only something clearly elsewhere (not the Echo we follow, measured a moment
		// later) and not near the player or camera.
		if (aAwayFromAnchor && (FlatDistance(cl.Center(), aAnchor) < 12.0f || FlatDistance(cl.Center(), aPlayer) < 8.0f)) { continue; }
		// The next dash starts where the Echo stood (detected within ~0.5 s, so a few metres on).
		if (aHaveAnchor && !aAwayFromAnchor && FlatDistance(cl.Origin(), aAnchor) > 8.0f) { continue; }
		if (!aHaveAnchor && FlatDistance(cl.Center(), aPlayer) < 12.0f) { continue; }
		// The very first dash starts at the Echo's spawn (identical in every attempt): its line must
		// run back through the spawn point. Rules out adds and messengers at the start.
		if (!aHaveAnchor && !mReacquiring)
		{
			const Vec3 back = Flat(Vec3{ ArenaOutline::SPAWN_X, 0, ArenaOutline::SPAWN_Z } - cl.Center());
			const float along = Dot(back, cl.Dir());           // negative: spawn lies behind
			const float sideways = Length(back - cl.Dir() * along);
			if (along > 2.0f || sideways > 3.0f) { continue; }
		}
		ok.push_back(cl);
	}
	if (ok.empty()) { return false; }
	// Biggest cluster first; speed closest to the Echo's as tie-break.
	std::sort(ok.begin(), ok.end(), [&](const Cluster& a, const Cluster& b) {
		if (a.Members.size() != b.Members.size()) { return a.Members.size() > b.Members.size(); }
		return std::fabs(a.Speed() - 10.4f) < std::fabs(b.Speed() - 10.4f);
	});

	std::string line = aAwayFromAnchor ? "Correction, switching to:" : aHaveAnchor ? "Next dash:" : "First dash, candidates:";
	for (size_t i = 0; i < ok.size() && i < 4; i++)
	{
		char b[160];
		std::snprintf(b, sizeof(b), " [%zu values, %.1f m/s, %.0f m from you at (%.0f, %.0f), heading at you %.2f, started %.1f m from the Echo]",
			ok[i].Members.size(), ok[i].Speed(), FlatDistance(ok[i].Center(), aPlayer), ok[i].Center().x, ok[i].Center().z,
			towardPlayer(ok[i]), aHaveAnchor ? FlatDistance(ok[i].Origin(), aAnchor) : -1.0f);
		line += b;
	}
	APIDefs->Log(LOGL_INFO, ADDON_NAME, line.c_str());
	const Cluster& best = ok.front();

	mLockedSize = best.Members.size();
	std::vector<Cand> members = best.Members;
	if (members.size() > 48) { members.resize(48); }
	{ std::lock_guard<std::mutex> l(mClusterMutex); mCluster = std::move(members); }
	mLost = false;
	mFrozen = false;
	if (mStage != 3)
	{
		char b[128];
		std::snprintf(b, sizeof(b), "Locked onto the Echo, %.1f s after the pull.", Now() - mFightStart);
		APIDefs->Log(LOGL_INFO, ADDON_NAME, b);
	}
	mStage = 3;
	return true;
}

// ------------------------------------------------------------------------------------ Test mode

bool LiveEcho::CharInfo(uintptr_t aObj, uint32_t& aId) const
{
	uint64_t vt = 0, agent = 0;
	uint32_t id = 0;
	if (!GameMemory::Read(aObj, vt) || vt != mVtable) { return false; }
	if (!GameMemory::Read(aObj + mLayout.AgentPtrOffset, agent) || !GameMemory::LooksLikeHeapPtr(agent)) { return false; }
	if (!GameMemory::Read((uintptr_t)agent + mLayout.AgentIdOffset, id) || id == 0 || id > 0xFFFF) { return false; }
	aId = id;
	return true;
}

uintptr_t LiveEcho::SearchInst(uint16_t aInstId)
{
	std::vector<uintptr_t> objs;
	GameMemory::FindQword(GameMemory::ScannableRegions(), mVtable, objs, nullptr,
		[this, aInstId]() { return mStop.load() || mWantedInstId.load() != aInstId; });
	for (uintptr_t o : objs) { uint32_t id; if (CharInfo(o, id) && id == aInstId) { return o; } }
	return 0;
}

// ------------------------------------------------------------------------------------ threads

void LiveEcho::WorkerLoop()
{
	while (!mStop)
	{
		// Fast while watching the values where the Echo stands, otherwise relaxed.
		std::this_thread::sleep_for(std::chrono::milliseconds(mStage == 3 && mFrozen && !mStanding.empty() ? 15 : 50));
		const double now = Now();

		if (!EchoMode())
		{
			const uint16_t wanted = mWantedInstId;
			if (wanted == 0 || mTestObj != 0 || wanted == mDeadInstId) { continue; }
			const uintptr_t obj = SearchInst(wanted);
			if (obj && mWantedInstId == wanted) { mTestObj = obj; } else { mDeadInstId = wanted; }
			continue;
		}

		if (!mInFight)
		{
			if (mOutOfFightSince == 0) { mOutOfFightSince = now; }
			if (now - mOutOfFightSince > 3.0 && mStage != 0) { ResetFight(); }
			continue;
		}
		if (mOutOfFightSince != 0 || mStage == 0) { mFightStart = now; }
		mOutOfFightSince = 0;

		if (mStage == 0) { ScanArena(PlayerPos(), false); }
		else if (mStage == 2)
		{
			// The Echo's dash memory only exists once the dash has started (~2 s after the pull),
			// so keep re-scanning until a dash shows up.
			if (!FindEchoCluster(PlayerPos(), false, Vec3{}, MIN_ECHO_VALUES, false) && now - mLastScan > 1.0) { ScanArena(PlayerPos(), true); }
		}
		else if (mStage == 3)
		{
			if (mLost) { mStage = 2; continue; }
			if (mFrozen)
			{
				Vec3 anchor;
				{ std::lock_guard<std::mutex> l(mClusterMutex); anchor = mAnchor; }
				if (mFrozenSince == 0)
				{
					// Just stopped at a wall: watch the values sitting where the Echo stands right away,
					// and refresh the scan in the background (the next dash may use new memory).
					mFrozenSince = now;
					BuildStanding(anchor);
					if (!mScanJob.valid())
					{
						const Vec3 c = PlayerPos();
						mScanJob = std::async(std::launch::async, [this, c]() { double s; return DoScan(c, s); });
					}
				}
				else if (FlatDistance(anchor, mStandingAt) > 1.5f)
				{
					BuildStanding(anchor);
				}

				// Background scan finished: take its values and pick the standing ones again.
				if (mScanJob.valid() && mScanJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
				{
					std::vector<Cand> fresh = mScanJob.get();
					mLastScan = Now();
					if (!fresh.empty()) { mCands = std::move(fresh); BuildStanding(anchor); }
				}

				// Fast path: the standing values start moving the moment the dash begins.
				if (WatchStanding()) { mFrozenSince = 0; continue; }

				// Slow path (backup): search all values for a dash starting here. Only when the fast
				// path has nothing to watch, or the Echo has been standing unusually long.
				const bool fastUsable = mStanding.size() >= 3 && now - mFrozenSince < 2.5;
				if (fastUsable || mScanJob.valid() || now - mLastSlowCheck < 0.6) { continue; }
				mLastSlowCheck = now;
				if (FindEchoCluster(PlayerPos(), true, anchor, MIN_ECHO_VALUES, false)) { mFrozenSince = 0; mStanding.clear(); }
				else if (Now() - mFrozenSince > 2.0)
				{
					// The Echo never stands that long: we were following something else. Re-acquire
					// the best Echo-speed dash anywhere (first-dash rules, no spawn learning).
					mReacquiring = true;
					const bool found = FindEchoCluster(PlayerPos(), false, Vec3{}, MIN_ECHO_VALUES, false);
					mReacquiring = false;
					if (found)
					{
											mFrozenSince = 0;
						APIDefs->Log(LOGL_INFO, ADDON_NAME, "Re-acquired the Echo (previous lock didn't continue).");
					}
				}
			}
			else
			{
				mFrozenSince = 0;
				// During a dash: if something much bigger is dashing elsewhere at the Echo's speed,
				// we are following the wrong thing (e.g. a messenger): switch.
				if (now - mLastCorrection > 1.0)
				{
					mLastCorrection = now;
					Vec3 at;
					{ std::lock_guard<std::mutex> l(mClusterMutex); at = mAnchor; }
					mReacquiring = true;
					FindEchoCluster(PlayerPos(), true, at, std::max<size_t>(50, mLockedSize * 3), true);
					mReacquiring = false;
								}
			}
		}
	}
}

EchoState LiveEcho::Update(const Vec3& aPlayer, float aDt)
{
	EchoState s;
	const double t = Now();
	Vec3 pos;

	if (EchoMode())
	{
		mInFight = MumbleLink && MumbleLink->Context.MapID == MAP_HALL_OF_CHAINS && MumbleLink->Context.IsInCombat;

		std::vector<Cand> cluster;
		if (mStage == 3) { std::lock_guard<std::mutex> l(mClusterMutex); cluster = mCluster; }
		if (cluster.empty() || mLost)
		{
			mHaveLastPos = false;
			mHaveSmooth = false;
			mSamples.clear();
			switch (mStage.load())
			{
				case 1:  mStatus = "Fight started, scanning memory..."; break;
				case 2:  mStatus = "Waiting for the Echo's dash..."; break;
				default: mStatus = "Waiting for the fight to start."; break;
			}
			return s;
		}

		// Median of the cluster members; drop members that drifted away from it.
		std::vector<Vec3> ps;
		std::vector<Cand> keep;
		for (auto& c : cluster) { Vec3 p; if (ReadCand(c.Addr, c.Enc, p) && FlatDistance(p, aPlayer) < 200.0f) { ps.push_back(p); keep.push_back(c); } }
		// Memory reused for something else after the dash: the Echo can't move 15 m in one frame.
		if (!ps.empty() && mHaveLastPos)
		{
			Vec3 avg;
			for (auto& p : ps) { avg = avg + p; }
			avg = avg * (1.0f / ps.size());
			if (FlatDistance(avg, mLastPos) > 15.0f) { ps.clear(); }
		}
		if (ps.empty())
		{
			// Values gone (freed after the dash): keep showing the last position until the next dash.
			if (!mHaveLastPos) { mLost = true; return s; }
			mFrozen = true;
			s.Valid = true;
			s.Position = mLastPos;
			s.HasGround = true;
			s.GroundY = ArenaOutline::FLOOR_Y;
			mStatus = "Tracking the Echo (at the wall).";
			return s;
		}
		auto median = [&](float Vec3::* m) {
			std::vector<float> v; for (auto& p : ps) { v.push_back(p.*m); }
			std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
			return v[v.size() / 2];
		};
		pos = { median(&Vec3::x), median(&Vec3::y), median(&Vec3::z) };
		// Average of the members that agree with the median (steadier than the median itself).
		std::vector<Cand> good;
		Vec3 sum;
		for (size_t i = 0; i < ps.size(); i++) { if (FlatDistance(ps[i], pos) < 3.0f) { good.push_back(keep[i]); sum = sum + ps[i]; } }
		if (!good.empty()) { pos = sum * (1.0f / good.size()); }
		if (good.size() != cluster.size())
		{
			std::lock_guard<std::mutex> l(mClusterMutex);
			mCluster = good;
		}
		if (good.empty()) { mLost = true; return s; }

		// Standing still (at a wall) for a moment: tell the worker to watch for the next dash.
		if (!mHaveLastPos || FlatDistance(pos, mLastPos) > 0.02f) { mStillSince = t; }
		mFrozen = t - mStillSince > 0.15; // ~1.5 m of travel during a dash
		{ std::lock_guard<std::mutex> l(mClusterMutex); mAnchor = pos; }
		mLastPos = pos;
		mHaveLastPos = true;
		mStatus = mFrozen ? "Tracking the Echo (at the wall)." : "Tracking the Echo.";
	}
	else
	{
		mInFight = false;
		const uint16_t inst = ArcAgents::FindNewest(mSpecies);
		if (inst != mWantedInstId) { mWantedInstId = inst; mTestObj = 0; mSamples.clear(); }
		if (inst == 0) { mStatus = "Waiting for ArcDPS to report the NPC."; return s; }
		const uintptr_t obj = mTestObj;
		if (obj == 0) { mStatus = "NPC reported, searching memory..."; return s; }
		uint32_t id; float raw[3];
		if (!CharInfo(obj, id) || id != inst || !GameMemory::Read(obj + mLayout.PosOffset, raw, sizeof(raw)))
		{
			mTestObj = 0;
			mStatus = "Lost the NPC, searching again...";
			return s;
		}
		pos = GameMemory::GameToMumble(raw);
		if (FlatDistance(pos, aPlayer) > 300.0f) { mStatus = "NPC position looks wrong."; return s; }
		mStatus = "Tracking the test NPC.";
	}

	// Velocity over ~0.3 s (steady direction and colours).
	mSamples.push_back({ t, pos });
	while (mSamples.size() > 2 && t - mSamples[1].T > 0.3) { mSamples.erase(mSamples.begin()); }
	Vec3 vel;
	const Sample& old = mSamples.front();
	if (t - old.T > 0.05) { vel = (pos - old.P) * (float)(1.0 / (t - old.T)); }

	// Smoothed position: predict along the velocity, then blend towards the measurement.
	// Snap on big jumps (new lock, dash start) so it never lags behind.
	if (!mHaveSmooth || FlatDistance(pos, mSmooth) > 5.0f)
	{
		mSmooth = pos;
		mHaveSmooth = true;
	}
	else
	{
		const float dt = std::clamp(aDt, 0.0f, 0.1f);
		const Vec3 predicted = mSmooth + vel * dt;
		const float k = 1.0f - std::exp(-dt / 0.08f);
		mSmooth = predicted + (pos - predicted) * k;
	}

	// During a dash the Echo runs in a straight line at constant speed: fit a line through the
	// positions of the last 1.5 s and use it (no jitter). Restart the fit if a measurement strays.
	if (EchoMode())
	{
		if (mFrozen) { mFit.clear(); }
		else
		{
			mFit.push_back({ t, pos });
			while (mFit.size() > 2 && t - mFit.front().T > 1.5) { mFit.erase(mFit.begin()); }
			if (mFit.size() >= 5 && t - mFit.front().T > 0.12)
			{
				const double t0 = mFit.front().T;
				double st = 0, sx = 0, sz = 0, stt = 0, stx = 0, stz = 0;
				for (auto& f : mFit)
				{
					const double tt = f.T - t0;
					st += tt; sx += f.P.x; sz += f.P.z; stt += tt * tt; stx += tt * f.P.x; stz += tt * f.P.z;
				}
				const double n = (double)mFit.size(), den = n * stt - st * st;
				if (den > 1e-9)
				{
					const double bx = (n * stx - st * sx) / den, bz = (n * stz - st * sz) / den;
					const double ax = (sx - bx * st) / n, az = (sz - bz * st) / n;
					const Vec3 fitPos{ (float)(ax + bx * (t - t0)), pos.y, (float)(az + bz * (t - t0)) };
					if (FlatDistance(fitPos, pos) > 2.0f)
					{
						mFit.clear();
						mFit.push_back({ t, pos });
					}
					else
					{
						mSmooth = fitPos;
						vel = Vec3{ (float)bx, 0.0f, (float)bz };
					}
				}
			}
		}
	}

	s.Valid = true;
	s.Position = mSmooth;
	s.Velocity = vel;
	if (EchoMode())
	{
		// Draw on the arena floor, whatever height the player or the tracked values are at.
		s.HasGround = true;
		s.GroundY = ArenaOutline::FLOOR_Y;
	}

	// Where the current dash ends: the learned arena edge along the direction of travel.
	const float speed = Length(Flat(vel));
	float tEnd;
	if (EchoMode() && speed > 3.0f && ArenaOutline::Exit(mSmooth, Normalize(Flat(vel)), tEnd))
	{
		s.HasPathEnd = true;
		s.PathEnd = mSmooth + Normalize(Flat(vel)) * tEnd;
	}
	return s;
}
