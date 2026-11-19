#pragma once
// Fixed outline of the Dhuum CM arena, in MumbleLink metres (x, z): where Ender's Echo's
// position is when a dash ends at the wall (so pillars and the platform in front of Dhuum are
// included).
//
// Built offline from 61,763 Echo positions / 2,040 dash ends in 111 ArcDPS logs: for each of 180
// directions (2 degrees) around the arena centre, the furthest the Echo got. Nothing is learned
// at runtime.

#include "Geometry.h"

namespace ArenaOutline
{
	struct Pt { float X, Z; };

	/// Where the Echo stands before its first dash (identical in all 111 logs).
	constexpr float SPAWN_X = 439.48f;
	constexpr float SPAWN_Z = 33.91f;

	/// Height of the (flat) arena floor in MumbleLink metres: players' and the Echo's median height
	/// in the logs. Players float up to ~206 m doing greens; the Echo stays at 157.9-159.7 m.
	constexpr float FLOOR_Y = 157.86f;

	constexpr Pt POINTS[] = {
		{ 351.13f, 17.56f }, { 351.01f, 15.91f }, { 350.91f, 14.24f }, { 350.84f, 12.56f },
		{ 350.69f, 10.85f }, { 350.50f, 9.11f }, { 350.53f, 7.37f }, { 351.12f, 5.76f },
		{ 352.53f, 4.42f }, { 353.71f, 3.06f }, { 353.93f, 1.39f }, { 353.39f, -0.64f },
		{ 352.94f, -2.72f }, { 352.41f, -4.95f }, { 352.55f, -6.92f }, { 352.82f, -8.88f },
		{ 353.38f, -10.72f }, { 353.70f, -12.78f }, { 354.25f, -14.73f }, { 355.51f, -16.19f },
		{ 356.85f, -17.56f }, { 358.17f, -18.94f }, { 359.35f, -20.46f }, { 360.54f, -22.00f },
		{ 362.68f, -22.48f }, { 364.89f, -22.75f }, { 367.07f, -22.92f }, { 369.09f, -23.19f },
		{ 371.21f, -23.16f }, { 373.03f, -23.51f }, { 374.92f, -23.61f }, { 376.95f, -23.30f },
		{ 378.69f, -23.43f }, { 380.37f, -23.62f }, { 381.76f, -24.42f }, { 382.65f, -26.75f },
		{ 383.39f, -30.03f }, { 385.15f, -30.30f }, { 386.97f, -30.24f }, { 388.71f, -30.39f },
		{ 390.47f, -30.36f }, { 392.21f, -30.36f }, { 393.93f, -30.29f }, { 395.64f, -30.27f },
		{ 397.34f, -30.19f }, { 399.04f, -30.26f }, { 400.75f, -30.47f }, { 402.49f, -30.72f },
		{ 404.26f, -31.02f }, { 406.05f, -31.24f }, { 407.87f, -31.41f }, { 409.73f, -31.59f },
		{ 411.56f, -31.51f }, { 413.43f, -31.47f }, { 414.23f, -28.21f }, { 415.30f, -26.20f },
		{ 416.84f, -25.56f }, { 418.49f, -25.16f }, { 420.71f, -25.81f }, { 422.87f, -26.15f },
		{ 424.97f, -26.18f }, { 427.08f, -26.10f }, { 429.40f, -26.20f }, { 431.48f, -25.80f },
		{ 434.08f, -25.94f }, { 436.65f, -25.87f }, { 438.92f, -25.30f }, { 439.86f, -23.29f },
		{ 440.16f, -20.76f }, { 441.10f, -18.93f }, { 441.40f, -16.61f }, { 442.09f, -14.70f },
		{ 442.66f, -12.76f }, { 442.92f, -10.67f }, { 443.34f, -8.75f }, { 443.71f, -6.85f },
		{ 443.72f, -4.82f }, { 443.77f, -2.88f }, { 443.91f, -1.03f }, { 444.30f, 0.68f },
		{ 443.54f, 2.76f }, { 442.23f, 4.91f }, { 441.54f, 6.76f }, { 440.73f, 8.56f },
		{ 440.31f, 10.19f }, { 439.88f, 11.77f }, { 439.90f, 13.26f }, { 439.85f, 14.73f },
		{ 439.74f, 16.20f }, { 439.94f, 17.65f }, { 439.99f, 19.11f }, { 440.50f, 20.60f },
		{ 440.83f, 22.11f }, { 441.36f, 23.68f }, { 441.66f, 25.26f }, { 442.83f, 27.05f },
		{ 443.94f, 28.94f }, { 445.66f, 31.10f }, { 445.52f, 32.85f }, { 445.64f, 34.72f },
		{ 445.78f, 36.64f }, { 445.88f, 38.62f }, { 445.96f, 40.65f }, { 445.79f, 42.63f },
		{ 445.34f, 44.51f }, { 445.20f, 46.62f }, { 444.94f, 48.73f }, { 444.78f, 51.00f },
		{ 444.47f, 53.25f }, { 443.96f, 55.44f }, { 443.26f, 57.56f }, { 442.01f, 59.24f },
		{ 439.35f, 59.53f }, { 436.73f, 59.71f }, { 434.28f, 59.89f }, { 432.00f, 60.13f },
		{ 429.63f, 60.09f }, { 427.64f, 60.44f }, { 425.66f, 60.68f }, { 423.23f, 60.05f },
		{ 421.44f, 60.32f }, { 420.02f, 61.21f }, { 419.16f, 63.35f }, { 418.67f, 66.63f },
		{ 416.92f, 67.15f }, { 415.00f, 67.18f }, { 413.09f, 67.11f }, { 411.28f, 67.24f },
		{ 409.47f, 67.25f }, { 407.69f, 67.22f }, { 405.90f, 67.04f }, { 404.16f, 66.97f },
		{ 402.45f, 67.00f }, { 400.75f, 67.25f }, { 399.05f, 67.51f }, { 397.33f, 67.73f },
		{ 395.60f, 67.82f }, { 393.85f, 68.06f }, { 392.08f, 68.18f }, { 390.26f, 68.43f },
		{ 388.49f, 68.30f }, { 387.26f, 65.72f }, { 386.27f, 62.87f }, { 384.78f, 62.26f },
		{ 383.21f, 61.89f }, { 381.29f, 62.42f }, { 379.46f, 62.50f }, { 377.47f, 62.81f },
		{ 375.63f, 62.65f }, { 373.72f, 62.53f }, { 371.52f, 62.77f }, { 369.37f, 62.76f },
		{ 367.00f, 62.92f }, { 364.85f, 62.63f }, { 363.40f, 61.34f }, { 362.15f, 59.83f },
		{ 360.71f, 58.57f }, { 359.25f, 57.32f }, { 357.82f, 56.03f }, { 356.97f, 54.21f },
		{ 356.64f, 52.03f }, { 355.76f, 50.35f }, { 355.25f, 48.45f }, { 355.07f, 46.38f },
		{ 355.25f, 44.18f }, { 355.62f, 41.98f }, { 356.07f, 39.84f }, { 355.49f, 38.29f },
		{ 354.47f, 36.94f }, { 352.79f, 35.81f }, { 351.83f, 34.34f }, { 351.68f, 32.60f },
		{ 351.64f, 30.85f }, { 351.64f, 29.12f }, { 351.56f, 27.44f }, { 351.57f, 25.76f },
		{ 351.43f, 24.12f }, { 351.38f, 22.47f }, { 351.31f, 20.83f }, { 351.24f, 19.20f },
	};
	constexpr int COUNT = (int)(sizeof(POINTS) / sizeof(POINTS[0]));

	/// Distance along aDir (unit, flat) from aPos to the outline. False if there is no crossing ahead.
	inline bool Exit(const Vec3& aPos, const Vec3& aDir, float& aOutT)
	{
		float best = 1e9f;
		for (int i = 0; i < COUNT; i++)
		{
			const Pt& a = POINTS[i];
			const Pt& b = POINTS[(i + 1) % COUNT];
			// Ray p + t*d against segment a + u*(b - a).
			const float ex = b.X - a.X, ez = b.Z - a.Z;
			const float den = aDir.x * ez - aDir.z * ex;
			if (den > -1e-6f && den < 1e-6f) { continue; }
			const float wx = a.X - aPos.x, wz = a.Z - aPos.z;
			const float t = (wx * ez - wz * ex) / den;
			const float u = (wx * aDir.z - wz * aDir.x) / den;
			if (t > 0.0f && u >= 0.0f && u <= 1.0f && t < best) { best = t; }
		}
		if (best > 500.0f) { return false; }
		aOutT = best;
		return true;
	}
}
