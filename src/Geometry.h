#pragma once
// Small vector math in MumbleLink space: metres, left-handed, +Y up.

#include <cmath>

struct Vec3
{
	float x = 0, y = 0, z = 0;

	Vec3() = default;
	Vec3(float aX, float aY, float aZ) : x(aX), y(aY), z(aZ) {}

	Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
	Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
	Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b)
{
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3& v)
{
	float l = Length(v);
	return l > 1e-6f ? v * (1.0f / l) : Vec3{};
}
// Ground-plane (XZ) helpers; height is ignored for the danger logic.
inline Vec3 Flat(const Vec3& v) { return { v.x, 0.0f, v.z }; }
inline float FlatDistance(const Vec3& a, const Vec3& b) { return Length(Flat(a - b)); }

constexpr float INCH_TO_METRE = 0.0254f;

/// Ground-plane distance along aDir (unit, flat) from aPos until it leaves the circle (aCenter, aRadius).
/// Returns false if there is no exit ahead.
inline bool RayCircleExit(const Vec3& aPos, const Vec3& aDir, const Vec3& aCenter, float aRadius, float& aOutT)
{
	const Vec3 m = Flat(aPos - aCenter);
	const float b = Dot(m, aDir);
	const float c = Dot(m, m) - aRadius * aRadius;
	const float disc = b * b - c;
	if (disc < 0) { return false; }
	const float t = -b + std::sqrt(disc);
	if (t <= 0) { return false; }
	aOutT = t;
	return true;
}

/// Perspective camera built from MumbleLink camera data each frame.
struct Camera
{
	Vec3  Pos;
	Vec3  Right, Up, Fwd;
	float TanHalfFovY = 0.6f;
	float Aspect      = 16.0f / 9.0f;
	float Width = 0, Height = 0;

	void Set(const Vec3& aPos, const Vec3& aFront, float aFovY, float aWidth, float aHeight)
	{
		Pos    = aPos;
		Fwd    = Normalize(aFront);
		Right  = Normalize(Cross(Vec3{ 0, 1, 0 }, Fwd));
		Up     = Cross(Fwd, Right);
		TanHalfFovY = std::tan(aFovY * 0.5f);
		Width  = aWidth;
		Height = aHeight;
		Aspect = aHeight > 0 ? aWidth / aHeight : 1.0f;
	}

	/// Returns false when the point is behind (or too close to) the camera.
	bool WorldToScreen(const Vec3& aWorld, float& aOutX, float& aOutY) const
	{
		Vec3  d = aWorld - Pos;
		float z = Dot(d, Fwd);
		if (z < 0.1f) { return false; }
		float x = Dot(d, Right) / (z * TanHalfFovY * Aspect);
		float y = Dot(d, Up) / (z * TanHalfFovY);
		aOutX = (x + 1.0f) * 0.5f * Width;
		aOutY = (1.0f - y) * 0.5f * Height;
		return true;
	}
};
