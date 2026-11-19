#include "SimulatedEcho.h"

#include <random>

void SimulatedEcho::Start(const Vec3& aPlayer)
{
	static std::mt19937 rng{ std::random_device{}() };
	std::uniform_real_distribution<float> angle(0.0f, 6.2831853f);

	float a  = angle(rng);
	mCenter  = aPlayer;
	mPos     = aPlayer + Vec3{ std::cos(a), 0, std::sin(a) } * ArenaRadius;
	mPhase   = Phase::Waiting;
	mTimer   = WaitToCast;
	mActive  = true;
}

EchoState SimulatedEcho::Update(const Vec3& aPlayer, float aDt)
{
	EchoState s;
	if (!mActive) { return s; }

	// Keep the Echo at the player's height so the arrow and circle stay level.
	mPos.y    = aPlayer.y;
	mCenter.y = aPlayer.y;

	mTimer -= aDt;
	switch (mPhase)
	{
		case Phase::Waiting:
			if (mTimer <= 0)
			{
				// "Cast": lock the direction towards the player's current position.
				mDir   = Normalize(Flat(aPlayer - mPos));
				mPhase = Phase::Casting;
				mTimer = CastToRun;
			}
			break;

		case Phase::Casting:
			if (mTimer <= 0) { mPhase = Phase::Running; }
			break;

		case Phase::Running:
		{
			mPos = mPos + mDir * (Speed * aDt);
			Vec3 fromCenter = Flat(mPos - mCenter);
			if (Length(fromCenter) >= ArenaRadius && Dot(fromCenter, mDir) > 0)
			{
				// Hit the wall: clamp onto it and wait for the next cast.
				mPos   = mCenter + Normalize(fromCenter) * ArenaRadius;
				mPos.y = aPlayer.y;
				mPhase = Phase::Waiting;
				mTimer = WaitToCast;
			}
			break;
		}
	}

	s.Valid    = true;
	s.Position = mPos;
	s.Velocity = mPhase == Phase::Running ? mDir * Speed : Vec3{};
	float t;
	if (mPhase == Phase::Running && RayCircleExit(mPos, mDir, mCenter, ArenaRadius, t))
	{
		s.HasPathEnd = true;
		s.PathEnd = mPos + mDir * t;
	}
	return s;
}
