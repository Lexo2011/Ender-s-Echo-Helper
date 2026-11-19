#pragma once
// Finds where the game keeps the Echo's moving (per-frame) position, without ArcDPS.
//
// The agent position (+0x108) only updates when the Echo's dash ends, and ArcDPS often reports
// the Echo too late in a solo attempt (the fight ends ~20 s in when a reaper dies). So:
//   1. when the player enters combat in Hall of Chains: scan memory for every position inside
//      the arena, and collect all character objects in the same pass;
//   2. keep candidates that move on the ground at 7-14 m/s in a steady direction, away from the
//      player (solo, only the Echo and its effects move like that);
//   3. search pointer routes from every character's agent to those candidates: the character
//      with routes is the Echo.
// Results go to addons/EndersEcho/echo_hunt.txt.

#include "GameMemory.h"
#include "Geometry.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EchoHunt
{
public:
	~EchoHunt();

	void Start(const std::string& aLogPath, const GameMemory::Layout& aLayout);
	void Stop();
	bool IsActive() const { return mStage != Stage::Off; }
	std::string Status() const;

	/// Called every frame while active.
	void Feed(const Vec3& aPlayer, bool aInCombat, unsigned aMapId, double aTime);

private:
	enum class Stage { Off, Armed, Scanning, Motion, Routes, Sampling, Done };

	struct Cand { uintptr_t Addr; uint8_t Enc; Vec3 Stored; int Steps = 0; int Samples = 0; Vec3 Last; bool HaveLast = false; };

	void Run(Vec3 aCenter);
	void Scan(Vec3 aCenter);
	bool MotionFilter();
	void RouteSearch();
	void ReadAll(const std::vector<Cand>& aIn, std::vector<Vec3>& aPos, std::vector<double>& aTime, std::vector<char>& aOk);
	void SampleCandidates();
	void LogSurvivors(const char* aTitle);
	void Log(const std::string& aLine);

	std::atomic<Stage>     mStage{ Stage::Off };
	std::string            mLogPath;
	GameMemory::Layout     mLayout{};
	std::thread            mWorker;
	std::atomic<bool>      mCancel{ false };

	mutable std::mutex     mMutex;
	std::vector<Cand>      mCands;
	std::vector<uintptr_t> mChars;   // character (position) objects found during the scan

	double mSampleLast = 0;
	int    mSampleTicks = 0;
};
