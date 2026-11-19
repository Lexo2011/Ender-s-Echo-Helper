#include "Settings.h"

#include <cstdio>
#include <fstream>
#include <sstream>

// Plain "key=value" lines. Unknown keys are ignored, missing keys keep their defaults.

namespace
{
	template <typename F>
	void ForEachField(Settings& s, F&& f)
	{
		f("enabled", s.Enabled);
		f("only_in_hall_of_chains", s.OnlyInHallOfChains);
		f("show_status_text", s.ShowStatusText);
		f("show_arrow_distance", s.ShowArrowDistance);
		f("timer_text_size", s.TimerTextSize);
		f("distance_text_size", s.DistanceTextSize);
		f("show_path", s.ShowPath);
		f("path_style", s.PathStyle);
		f("path_thickness", s.PathThickness);
		f("col_path", s.ColPath);
		f("test_species", s.TestSpecies);
		f("auto_mark", s.AutoMark);
		f("auto_mark_only_as_commander", s.AutoMarkOnlyAsCommander);
		f("circle_radius", s.CircleRadius);
		f("circle_thickness", s.CircleThickness);
		f("arrow_length", s.ArrowLength);
		f("arrow_width", s.ArrowWidth);
		f("danger_half_width", s.DangerHalfWidth);
		f("warn_half_width", s.WarnHalfWidth);
		f("moving_threshold", s.MovingThreshold);
		f("col_danger", s.ColDanger);
		f("col_warn", s.ColWarn);
		f("col_safe", s.ColSafe);
		f("col_idle", s.ColIdle);
	}

	void Write(std::ostream& o, bool v)  { o << (v ? 1 : 0); }
	void Write(std::ostream& o, float v) { o << v; }
	void Write(std::ostream& o, const float (&v)[4]) { o << v[0] << ' ' << v[1] << ' ' << v[2] << ' ' << v[3]; }

	void Write(std::ostream& o, int v) { o << v; }
	void Read(std::istream& i, int& v) { int x; if (i >> x) { v = x; } }
	void Read(std::istream& i, bool& v)  { int x; if (i >> x) { v = x != 0; } }
	void Read(std::istream& i, float& v) { float x; if (i >> x) { v = x; } }
	void Read(std::istream& i, float (&v)[4])
	{
		float t[4];
		if (i >> t[0] >> t[1] >> t[2] >> t[3]) { for (int k = 0; k < 4; k++) { v[k] = t[k]; } }
	}
}

void Settings::Load(const std::string& aPath)
{
	std::ifstream file(aPath);
	if (!file) { return; }

	std::string line;
	while (std::getline(file, line))
	{
		auto eq = line.find('=');
		if (eq == std::string::npos) { continue; }
		std::string key = line.substr(0, eq);
		std::istringstream value(line.substr(eq + 1));
		ForEachField(*this, [&](const char* aKey, auto& aField) {
			if (key == aKey) { Read(value, aField); }
		});
	}
}

void Settings::Save(const std::string& aPath) const
{
	std::ofstream file(aPath, std::ios::trunc);
	if (!file) { return; }
	ForEachField(const_cast<Settings&>(*this), [&](const char* aKey, auto& aField) {
		file << aKey << '=';
		Write(file, aField);
		file << '\n';
	});
}
