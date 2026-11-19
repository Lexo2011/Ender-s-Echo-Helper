#pragma once
// Live Ender's Echo source: reads the Echo's position from game memory.
//
// Dhuum (species = Ender's Echo, in Hall of Chains), no ArcDPS and no pointer routes needed:
//   1. at the pull, scan memory (multi-threaded) for every position-like value inside the arena;
//   2. keep the values that move on the ground in a straight line at running speed, group them
//      into clusters (one per moving thing) and pick the cluster closest to the Echo's dash speed
//      (400 in/s = 10.16 m/s);
//   3. track that cluster every frame (median of its members). If it falls apart, re-filter the
//      scanned values at the next dash (fast, no new scan).
//
// Test mode (any other species, any map): ArcDPS names the NPC (species -> agent id), its
// character object is found in memory and its agent position is read.

#include "ArenaModel.h"
#include "EchoSource.h"
#include "GameMemory.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LiveEcho : public IEchoSource
{
public:
	explicit LiveEcho(const GameMemory::Layout& aLayout);
	~LiveEcho() override;

	const char* Name() const override { return "Live"; }
	EchoState Update(const Vec3& aPlayer, float aDt) override;

	/// Short status line for the options panel.
	const char* Status() const { return mStatus; }

	/// Species to track. Ender's Echo by default; any NPC species for testing in open world.
	void SetSpecies(uint32_t aSpecies) { mSpecies = aSpecies; }

	/// File that remembers where the Echo spawns (learned from a confirmed first dash).
	void SetSpawnFile(const std::string& aPath);
	/// File that remembers the arena outline (learned from where dashes end).
	void SetArenaFile(const std::string& aPath) { mArena.Load(aPath); }
	int ArenaKnownDirections() const { return mArena.KnownBins(); }

private:
	static constexpr uint32_t SPECIES_ENDERS_ECHO_ID = 19628;
	struct Cand { uintptr_t Addr; uint8_t Enc; };

	void WorkerLoop();
	bool EchoMode() const { return mSpecies == SPECIES_ENDERS_ECHO_ID; }

	// Dhuum mode
	void ScanArena(const Vec3& aCenter, bool aKeepTracking);
	bool FindEchoCluster(const Vec3& aPlayer, bool aHaveAnchor, const Vec3& aAnchor, size_t aMinSize, bool aAwayFromAnchor);
	void ResetFight();

	// Test mode
	bool CharInfo(uintptr_t aObj, uint32_t& aId) const;
	uintptr_t SearchInst(uint16_t aInstId);

	const GameMemory::Layout mLayout;
	const uintptr_t          mVtable;
	std::atomic<uint32_t>    mSpecies{ SPECIES_ENDERS_ECHO_ID };

	// Shared between render and worker thread.
	std::atomic<bool>        mStop{ false };
	std::atomic<bool>        mInFight{ false };    // in Hall of Chains and in combat
	std::atomic<int>         mStage{ 0 };          // 0 idle, 1 scanning, 2 looking for the dash, 3 tracking
	std::atomic<bool>        mLost{ false };       // render -> worker: cluster fell apart
	std::atomic<bool>        mFrozen{ false };     // render -> worker: Echo stands still (at a wall)
	Vec3                     mAnchor;              // render -> worker: current Echo position (mClusterMutex)
	std::mutex               mClusterMutex;
	std::vector<Cand>        mCluster;             // the Echo's values
	std::atomic<uint16_t>    mWantedInstId{ 0 };   // test mode
	std::atomic<uint16_t>    mDeadInstId{ 0 };
	std::atomic<uintptr_t>   mTestObj{ 0 };
	std::thread              mWorker;

	// Worker thread only.
	std::vector<Cand>        mCands;               // every position-like value in the arena
	double                   mOutOfFightSince = 0;
	double                   mFightStart = 0;
	double                   mLastScan = 0;
	double                   mFrozenSince = 0;     // worker: when the Echo started standing still
	bool                     mReacquiring = false; // worker: mid-fight re-acquire (no spawn check)
	double                   mLastCorrection = 0;  // worker: last "bigger dash elsewhere" check
	std::string              mSpawnFile;
	Vec3                     mSpawn;              // where the Echo's first dash starts
	bool                     mHaveSpawn = false;
	Vec3                     mFirstOrigin;        // origin of this fight's first dash
	bool                     mFirstPending = false; // first dash not confirmed yet

	// Render thread only.
	struct Sample { double T; Vec3 P; };
	std::vector<Sample>      mSamples;
	Vec3                     mLastPos;
	bool                     mHaveLastPos = false;
	Vec3                     mSmooth;
	bool                     mHaveSmooth = false;
	std::vector<Sample>      mFit;                // positions of the current dash (straight-line fit)
	double                   mStillSince = 0;
	ArenaModel               mArena;
	bool                     mWasFrozen = true;
	Vec3                     mDashStart;
	const char*              mStatus = "Waiting for the Echo.";
};
