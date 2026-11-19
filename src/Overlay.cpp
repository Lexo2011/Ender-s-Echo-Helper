#include "Overlay.h"

#include "imgui.h"
#include "Shared.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

ThreatInfo Evaluate(const Settings& s, const Vec3& aPlayer, const EchoState& aEcho)
{
	ThreatInfo t;
	if (!aEcho.Valid) { return t; }

	Vec3  toPlayer = Flat(aPlayer - aEcho.Position);
	Vec3  vel      = Flat(aEcho.Velocity);
	float speed    = Length(vel);
	t.EchoDistance = Length(toPlayer);

	// Touching the Echo itself is always dangerous, moving or not.
	if (t.EchoDistance < s.DangerHalfWidth)
	{
		t.Level = Threat::Danger;
		t.PathDistance = 0;
		return t;
	}

	if (speed < s.MovingThreshold)
	{
		// Standing at the wall: its next direction depends on which player it picks.
		t.Level = Threat::Idle;
		return t;
	}

	// The Echo runs straight until the wall; the player is always inside the arena,
	// so the forward ray is a good model of its remaining path.
	Vec3  dir   = vel * (1.0f / speed);
	float along = Dot(toPlayer, dir);
	if (along <= 0)
	{
		t.Level = Threat::Safe; // it is moving away from you
		t.PathDistance = t.EchoDistance;
		return t;
	}

	// The predicted end of the dash is NOT used here: if it is too short, standing in the path
	// near the wall would look safe. The player is always inside the arena, so the whole line
	// ahead of the Echo counts.

	t.PathDistance = Length(toPlayer - dir * along);
	t.TimeToReach  = along / speed;

	if (t.PathDistance < s.DangerHalfWidth)    { t.Level = Threat::Danger; }
	else if (t.PathDistance < s.WarnHalfWidth) { t.Level = Threat::Warn; }
	else                                       { t.Level = Threat::Safe; }
	return t;
}

namespace
{
	ImU32 ColorFor(const Settings& s, Threat aLevel)
	{
		const float* c = s.ColIdle;
		switch (aLevel)
		{
			case Threat::Danger: c = s.ColDanger; break;
			case Threat::Warn:   c = s.ColWarn;   break;
			case Threat::Safe:   c = s.ColSafe;   break;
			default: break;
		}
		return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
	}

	/// Text with a dark outline, at a chosen pixel size. Uses Nexus' large font so it stays sharp.
	void DrawLabel(ImDrawList* dl, float aSize, ImVec2 aAnchor, bool aCenterY, ImU32 aCol, const char* aText)
	{
		ImFont* font = (NexusLink && NexusLink->FontBig) ? (ImFont*)NexusLink->FontBig : ImGui::GetFont();
		const ImVec2 size = font->CalcTextSizeA(aSize, FLT_MAX, 0.0f, aText);
		const ImVec2 pos(aAnchor.x - size.x * 0.5f, aCenterY ? aAnchor.y - size.y * 0.5f : aAnchor.y);
		const float o = aSize > 24.0f ? 2.0f : 1.0f;
		const ImU32 shadow = IM_COL32(0, 0, 0, 220);
		for (int dx = -1; dx <= 1; dx++)
		{
			for (int dy = -1; dy <= 1; dy++)
			{
				if (dx || dy) { dl->AddText(font, aSize, ImVec2(pos.x + dx * o, pos.y + dy * o), shadow, aText); }
			}
		}
		dl->AddText(font, aSize, pos, aCol, aText);
	}
}

void DrawOverlay(const Settings& s, const Camera& cam, const Vec3& aPlayer,
                 const EchoState& aEcho, const ThreatInfo& aThreat, const char* aSourceName)
{
	if (!aEcho.Valid) { return; }

	ImDrawList* dl  = ImGui::GetBackgroundDrawList();
	ImU32       col = ColorFor(s, aThreat.Level);

	// Everything on the ground is drawn at the arena floor height (live tracking), so it stays on
	// the ground while the player floats up (e.g. doing a green). Practice mode: the player's height.
	const float groundY = aEcho.HasGround ? aEcho.GroundY : aPlayer.y;
	const Vec3  feet{ aPlayer.x, groundY, aPlayer.z };

	// The Echo's path: a line on the ground from the Echo to where the dash ends (or 50 m).
	const Vec3 vel = Flat(aEcho.Velocity);
	if (s.ShowPath && Length(vel) > s.MovingThreshold)
	{
		const Vec3 start = Vec3{ aEcho.Position.x, groundY, aEcho.Position.z };
		const Vec3 dirP  = Normalize(vel);
		float len = aEcho.HasPathEnd ? FlatDistance(aEcho.PathEnd, aEcho.Position) : 50.0f;
		// Never draw the path shorter than where the player stands in it: the player is inside
		// the arena, so the dash certainly reaches that far.
		{
			const Vec3 toP = Flat(aPlayer - aEcho.Position);
			const float alongP = Dot(toP, dirP);
			const float sideP = Length(toP - dirP * alongP);
			if (alongP > 0 && sideP < s.WarnHalfWidth) { len = std::max(len, alongP + 3.0f); }
		}
		const ImU32 pcol = ImGui::ColorConvertFloat4ToU32(ImVec4(s.ColPath[0], s.ColPath[1], s.ColPath[2], s.ColPath[3]));
		const Vec3 side = Vec3{ dirP.z, 0, -dirP.x };
		auto withAlpha = [](const float* c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3] * a)); };

		// Projected in 1 m pieces, so it follows the ground in perspective and clips near the camera.
		for (float d = 0; d < len; d += 1.0f)
		{
			const float d2 = std::min(d + 1.0f, len);
			const Vec3 a = start + dirP * d, b = start + dirP * d2;
			if (s.PathStyle == 1)
			{
				// Band on the floor: the danger width filled, the warning width faint around it.
				auto quad = [&](float w0, float w1, ImU32 c) {
					ImVec2 p0, p1, p2, p3;
					if (cam.WorldToScreen(a + side * w0, p0.x, p0.y) && cam.WorldToScreen(b + side * w0, p1.x, p1.y) &&
					    cam.WorldToScreen(b + side * w1, p2.x, p2.y) && cam.WorldToScreen(a + side * w1, p3.x, p3.y))
					{
						dl->AddQuadFilled(p0, p1, p2, p3, c);
					}
				};
				const float dw = s.DangerHalfWidth, ww = s.WarnHalfWidth;
				quad(-dw, dw, withAlpha(s.ColPath, 0.45f));
				if (ww > dw)
				{
					quad(dw, ww, withAlpha(s.ColPath, 0.15f));
					quad(-ww, -dw, withAlpha(s.ColPath, 0.15f));
				}
				// edges of the danger width
				ImVec2 e0, e1;
				if (cam.WorldToScreen(a + side * dw, e0.x, e0.y) && cam.WorldToScreen(b + side * dw, e1.x, e1.y)) { dl->AddLine(e0, e1, pcol, s.PathThickness); }
				if (cam.WorldToScreen(a - side * dw, e0.x, e0.y) && cam.WorldToScreen(b - side * dw, e1.x, e1.y)) { dl->AddLine(e0, e1, pcol, s.PathThickness); }
			}
			else
			{
				ImVec2 p0, p1;
				if (cam.WorldToScreen(a, p0.x, p0.y) && cam.WorldToScreen(b, p1.x, p1.y)) { dl->AddLine(p0, p1, pcol, s.PathThickness); }
			}
		}
		// small bar across the end of the dash
		if (aEcho.HasPathEnd)
		{
			const Vec3 e = start + dirP * len;
			const float w = s.PathStyle == 1 ? s.DangerHalfWidth : 1.5f;
			ImVec2 a, b;
			if (cam.WorldToScreen(e + side * w, a.x, a.y) && cam.WorldToScreen(e - side * w, b.x, b.y)) { dl->AddLine(a, b, pcol, s.PathThickness); }
		}
	}

	// Ground circle around the player, projected segment by segment so it follows the camera tilt.
	constexpr int SEGMENTS = 64;
	ImVec2 prev;
	bool   prevOk = false;
	for (int i = 0; i <= SEGMENTS; i++)
	{
		float a = 6.2831853f * i / SEGMENTS;
		Vec3  p = feet + Vec3{ std::cos(a), 0, std::sin(a) } * s.CircleRadius;
		ImVec2 cur;
		bool   ok = cam.WorldToScreen(p, cur.x, cur.y);
		if (ok && prevOk) { dl->AddLine(prev, cur, col, s.CircleThickness); }
		prev = cur;
		prevOk = ok;
	}

	// Arrow on the ground pointing from the circle towards the Echo.
	Vec3 dir = Normalize(Flat(aEcho.Position - aPlayer));
	if (Length(dir) > 0)
	{
		Vec3 side = Vec3{ dir.z, 0, -dir.x } * (s.ArrowWidth * 0.5f);
		Vec3 base = feet + dir * s.CircleRadius;
		Vec3 tip  = feet + dir * (s.CircleRadius + s.ArrowLength);
		ImVec2 a, b, c;
		if (cam.WorldToScreen(tip, a.x, a.y) &&
		    cam.WorldToScreen(base + side, b.x, b.y) &&
		    cam.WorldToScreen(base - side, c.x, c.y))
		{
			dl->AddTriangleFilled(a, b, c, col);
		}

		if (s.ShowArrowDistance)
		{
			// Place the label a little beyond the tip, centred on that point.
			Vec3 labelAt = feet + dir * (s.CircleRadius + s.ArrowLength + 0.6f);
			ImVec2 p;
			if (cam.WorldToScreen(labelAt, p.x, p.y))
			{
				char buf[32];
				std::snprintf(buf, sizeof(buf), "%.0f m", aThreat.EchoDistance);
				DrawLabel(dl, s.DistanceTextSize, p, true, col, buf);
			}
		}
	}

	// Only when you're in the path: seconds until the Echo reaches you.
	if (s.ShowStatusText && aThreat.Level == Threat::Danger && aThreat.TimeToReach >= 0)
	{
		ImVec2 at;
		if (cam.WorldToScreen(aPlayer, at.x, at.y))
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.1fs", aThreat.TimeToReach);
			DrawLabel(dl, s.TimerTextSize, ImVec2(at.x, at.y + 20.0f), false, col, buf);
		}
	}
}
