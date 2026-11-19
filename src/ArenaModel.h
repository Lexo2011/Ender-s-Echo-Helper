#pragma once
// Learned outline of the arena (not assumed to be a circle).
//
// From a reference point inside the arena, the outline is stored as the distance at which the Echo
// stopped, in 36 directions of 10 degrees each (averaged). Directions without data are interpolated
// from their nearest known neighbours. Works for any shape whose edge is visible from the reference.

#include "Geometry.h"

#include <cmath>
#include <cstdio>
#include <string>

class ArenaModel
{
public:
	static constexpr int BINS = 36;

	// A point inside the Dhuum arena (MumbleLink metres), from observed wall hits.
	Vec3 Center{ 397.5f, 0.0f, 20.5f };

	void Load(const std::string& aPath)
	{
		mPath = aPath;
		if (FILE* f = std::fopen(aPath.c_str(), "r"))
		{
			for (int i = 0; i < BINS; i++)
			{
				if (std::fscanf(f, "%f %d", &mDist[i], &mCount[i]) != 2) { mDist[i] = 0; mCount[i] = 0; }
			}
			std::fclose(f);
		}
	}

	/// Records where a dash ended.
	void AddStop(const Vec3& aPos)
	{
		const float d = FlatDistance(aPos, Center);
		if (d < 30.0f || d > 120.0f) { return; } // only real wall hits (the arena is ~45 m across from the middle)
		// Keep the furthest stop per direction: the arena isn't round and the edge varies within a
		// direction, so erring on the long side is safer than ending the path short.
		const int b = Bin(aPos);
		if (d > mDist[b]) { mDist[b] = d; }
		mCount[b]++;
		Save();
	}

	int KnownBins() const { int k = 0; for (int i = 0; i < BINS; i++) { k += mCount[i] > 0; } return k; }

	/// Distance along aDir (unit, flat) from aPos to the learned edge. False until enough is known.
	bool Exit(const Vec3& aPos, const Vec3& aDir, float& aOutT) const
	{
		if (KnownBins() < 3) { return false; }
		for (float t = 0.5f; t < 150.0f; t += 0.5f)
		{
			const Vec3 q = aPos + aDir * t;
			float edge;
			if (EdgeAt(q, edge) && FlatDistance(q, Center) >= edge) { aOutT = t; return true; }
		}
		return false;
	}

private:
	int Bin(const Vec3& p) const
	{
		float a = std::atan2(p.z - Center.z, p.x - Center.x); // -pi..pi
		int b = (int)std::floor((a + 3.14159265f) / (2.0f * 3.14159265f) * BINS);
		return ((b % BINS) + BINS) % BINS;
	}

	/// Edge distance in the direction of p, interpolated between the nearest known bins.
	bool EdgeAt(const Vec3& p, float& aOut) const
	{
		const int b = Bin(p);
		if (mCount[b] > 0) { aOut = mDist[b]; return true; }
		for (int s = 1; s < BINS / 2; s++)
		{
			const int l = (b - s + BINS) % BINS, r = (b + s) % BINS;
			if (mCount[l] > 0 || mCount[r] > 0)
			{
				// nearest known on each side, weighted by angular distance
				int sl = s, sr = s;
				while (mCount[(b - sl + BINS) % BINS] == 0 && sl < BINS) { sl++; }
				while (mCount[(b + sr) % BINS] == 0 && sr < BINS) { sr++; }
				const float dl = mDist[(b - sl + BINS) % BINS], dr = mDist[(b + sr) % BINS];
				aOut = dl > dr ? dl : dr; // the larger neighbour: rather too long than too short
				return true;
			}
		}
		return false;
	}

	void Save() const
	{
		if (mPath.empty()) { return; }
		if (FILE* f = std::fopen(mPath.c_str(), "w"))
		{
			for (int i = 0; i < BINS; i++) { std::fprintf(f, "%f %d\n", mDist[i], mCount[i]); }
			std::fclose(f);
		}
	}

	std::string mPath;
	float mDist[BINS] = {};
	int   mCount[BINS] = {};
};
