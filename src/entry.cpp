///----------------------------------------------------------------------------------------------------
/// Ender's Echo Helper - Nexus addon for the Dhuum CM encounter.
/// Draws a circle around the player, coloured by how close the player is to Ender's Echo's
/// running path, with an arrow pointing towards the Echo and its dash drawn on the floor.
///
/// Build with -DENDERSECHO_DEV=ON for the development tools (memory diagnostic, Echo hunt,
/// test species, auto-mark). The default build is the version to share.
///----------------------------------------------------------------------------------------------------

#include "Shared.h"

#include "imgui.h"

#include "ArcAgents.h"
#include "GameMemory.h"
#include "LiveEcho.h"
#include "Overlay.h"
#include "Settings.h"
#include "SimulatedEcho.h"
#if ENDERSECHO_DEV
#include "Diagnostics.h"
#include "EchoHunt.h"
#endif

#include <algorithm>
#include <memory>
#include <string>

/* globals */
AddonAPI_t*       APIDefs        = nullptr;
NexusLinkData_t*  NexusLink      = nullptr;
Mumble::Data*     MumbleLink     = nullptr;
Mumble::Identity* MumbleIdentity = nullptr;

namespace
{
	constexpr const char* KB_TOGGLE_SIM = "KB_ENDERSECHO_TOGGLE_SIMULATION";
	constexpr const char* KB_TOGGLE     = "KB_ENDERSECHO_TOGGLE";

	AddonDefinition_t AddonDef = {};
	Settings          Config;
	std::string       SettingsPath;
	SimulatedEcho     Simulation;

	/// Source used during the real fight: reads the Echo from game memory.
	/// The Echo tracking itself doesn't depend on the game build; only the dev test mode does.
	std::unique_ptr<LiveEcho> Live;
	bool              LiveChecked = false;

	std::string AddonFile(const char* aName)
	{
		return std::string(APIDefs->Paths_GetAddonDirectory("EndersEcho")) + "/" + aName;
	}

	void EnsureLiveSource()
	{
		if (LiveChecked || !MumbleLink || MumbleLink->Context.BuildID == 0) { return; }
		LiveChecked = true;
		const GameMemory::Layout* lay = GameMemory::CurrentLayout(MumbleLink->Context.BuildID);
		const GameMemory::Layout none{};
		Live = std::make_unique<LiveEcho>(lay ? *lay : none);
	}

	Vec3 ToVec3(const Vector3& v) { return { v.X, v.Y, v.Z }; }

	void SaveSettings() { if (!SettingsPath.empty()) { Config.Save(SettingsPath); } }

#if ENDERSECHO_DEV
	EchoHunt Hunt;
	bool     ShowDiagnostics = false;

	/// The Echo exists for a whole attempt, so it is marked once and the flag resets when it disappears.
	/// Pressing the marker bind again on an already marked target would REMOVE the marker.
	bool MarkedThisAttempt = false;

	bool IsCommander() { return MumbleIdentity && MumbleIdentity->IsCommander; }

	void PressCrossAgentMarker() { APIDefs->GameBinds_InvokeAsync(GB_SquadMarkerSetAgent8, 50); }

	void UpdateAutoMark(const EchoState& aEcho)
	{
		if (!aEcho.Valid) { MarkedThisAttempt = false; return; }
		if (!Config.AutoMark || MarkedThisAttempt || !Live) { return; }
		if (Config.AutoMarkOnlyAsCommander && !IsCommander()) { return; }

		if (Live->CanMarkDirectly())
		{
			if (!Live->MarkEchoDirectly()) { return; }
		}
		else if (Live->IsEchoTargeted())
		{
			PressCrossAgentMarker();
		}
		else
		{
			return;
		}
		MarkedThisAttempt = true;
		APIDefs->Log(LOGL_INFO, ADDON_NAME, "Marked Ender's Echo with the X marker.");
	}
#endif

	void OnKeybind(const char* aIdentifier, bool aIsRelease)
	{
		if (aIsRelease) { return; }

		if (strcmp(aIdentifier, KB_TOGGLE_SIM) == 0)
		{
			if (Simulation.IsActive())
			{
				Simulation.Stop();
				APIDefs->GUI_SendAlert("Ender's Echo practice stopped");
			}
			else if (MumbleLink)
			{
				Simulation.Start(ToVec3(MumbleLink->AvatarPosition));
				APIDefs->GUI_SendAlert("Ender's Echo practice started");
			}
		}
		else if (strcmp(aIdentifier, KB_TOGGLE) == 0)
		{
			Config.Enabled = !Config.Enabled;
			SaveSettings();
		}
	}

	void AddonRender()
	{
		if (!MumbleLink || !NexusLink || !NexusLink->IsGameplay) { return; }

		const Vec3 player = ToVec3(MumbleLink->AvatarPosition);
#if ENDERSECHO_DEV
		Diagnostics::RenderWindow(&ShowDiagnostics, player);
		if (Hunt.IsActive())
		{
			Hunt.Feed(player, MumbleLink->Context.IsInCombat != 0, MumbleLink->Context.MapID, GetTickCount64() / 1000.0);
		}
		const bool testing = Config.TestSpecies > 0;
#else
		const bool testing = false;
#endif

		if (!Config.Enabled) { return; }
		const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.25f);

		EchoState   echo;
		const char* sourceName = nullptr;

		if (Simulation.IsActive())
		{
			echo = Simulation.Update(player, dt);
			sourceName = Simulation.Name();
		}
		else
		{
			EnsureLiveSource();
			if (Live)
			{
				if (!testing && Config.OnlyInHallOfChains && MumbleLink->Context.MapID != MAP_HALL_OF_CHAINS) { return; }
				Live->SetSpecies(testing ? (uint32_t)Config.TestSpecies : SPECIES_ENDERS_ECHO);
				echo = Live->Update(player, dt);
#if ENDERSECHO_DEV
				if (!testing) { UpdateAutoMark(echo); }
#endif
			}
		}

		if (!echo.Valid) { return; }

		Camera cam;
		const float fov = (MumbleIdentity && MumbleIdentity->FOV > 0.1f) ? MumbleIdentity->FOV : 1.222f;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		cam.Set(ToVec3(MumbleLink->CameraPosition), ToVec3(MumbleLink->CameraFront), fov, display.x, display.y);

		ThreatInfo threat = Evaluate(Config, player, echo);
		DrawOverlay(Config, cam, player, echo, threat, sourceName);
	}

	void ColorRow(const char* aLabel, float (&aColor)[4], bool& aChanged)
	{
		aChanged |= ImGui::ColorEdit4(aLabel, aColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
	}

	void AddonOptions()
	{
		bool changed = false;

		ImGui::TextDisabled("Ender's Echo Helper");
		changed |= ImGui::Checkbox("Enabled", &Config.Enabled);
		changed |= ImGui::Checkbox("Only in Hall of Chains", &Config.OnlyInHallOfChains);
		changed |= ImGui::Checkbox("Show time until hit (when red)", &Config.ShowStatusText);
		if (Config.ShowStatusText)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140);
			changed |= ImGui::SliderFloat("##timersize", &Config.TimerTextSize, 12.0f, 96.0f, "size %.0f px");
		}
		changed |= ImGui::Checkbox("Show distance (m) next to the arrow", &Config.ShowArrowDistance);
		if (Config.ShowArrowDistance)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140);
			changed |= ImGui::SliderFloat("##distsize", &Config.DistanceTextSize, 12.0f, 64.0f, "size %.0f px");
		}
		changed |= ImGui::Checkbox("Show the Echo's path", &Config.ShowPath);
		if (Config.ShowPath)
		{
			ImGui::SameLine();
			changed |= ImGui::RadioButton("Floor band", &Config.PathStyle, 1);
			ImGui::SameLine();
			changed |= ImGui::RadioButton("Line", &Config.PathStyle, 0);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120);
			changed |= ImGui::SliderFloat("##paththick", &Config.PathThickness, 1.0f, 10.0f, "%.0f px");
			ImGui::SameLine();
			ColorRow("Path colour", Config.ColPath, changed);
		}

		ImGui::Separator();
		ImGui::Text("Circle and arrow");
		changed |= ImGui::SliderFloat("Circle radius (m)", &Config.CircleRadius, 0.5f, 5.0f, "%.1f");
		changed |= ImGui::SliderFloat("Line thickness (px)", &Config.CircleThickness, 1.0f, 10.0f, "%.0f");
		changed |= ImGui::SliderFloat("Arrow length (m)", &Config.ArrowLength, 0.3f, 4.0f, "%.1f");
		changed |= ImGui::SliderFloat("Arrow width (m)", &Config.ArrowWidth, 0.2f, 3.0f, "%.1f");

		ImGui::Separator();
		ImGui::Text("Distance from the Echo's path");
		changed |= ImGui::SliderFloat("Red below (m)", &Config.DangerHalfWidth, 0.5f, 10.0f, "%.1f");
		changed |= ImGui::SliderFloat("Yellow below (m)", &Config.WarnHalfWidth, 1.0f, 20.0f, "%.1f");
		if (Config.WarnHalfWidth < Config.DangerHalfWidth) { Config.WarnHalfWidth = Config.DangerHalfWidth; }

		ColorRow("In path", Config.ColDanger, changed); ImGui::SameLine();
		ColorRow("Close", Config.ColWarn, changed);     ImGui::SameLine();
		ColorRow("Safe", Config.ColSafe, changed);      ImGui::SameLine();
		ColorRow("Echo waiting", Config.ColIdle, changed);

		ImGui::Separator();
		if (ImGui::Button(Simulation.IsActive() ? "Stop practice" : "Practice (fake Echo)"))
		{
			OnKeybind(KB_TOGGLE_SIM, false);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("A fake Echo dashes at you on a %.0f m arena. Keybind: ALT+SHIFT+E.", Simulation.ArenaRadius);
		if (Live) { ImGui::Text("Live tracking: %s", Live->Status()); }

#if ENDERSECHO_DEV
		ImGui::Separator();
		ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Developer tools");
		changed |= ImGui::Checkbox("Auto-mark the Echo with X (not functional yet)", &Config.AutoMark);
		if (Config.AutoMark)
		{
			ImGui::Indent();
			changed |= ImGui::Checkbox("Only while I am commander", &Config.AutoMarkOnlyAsCommander);
			if (ImGui::Button("Test: mark my current target with X")) { PressCrossAgentMarker(); }
			ImGui::Unindent();
		}
		bool hunt = Hunt.IsActive();
		if (ImGui::Checkbox("Find the Echo's moving position (Dhuum)", &hunt))
		{
			const GameMemory::Layout* lay = GameMemory::CurrentLayout(MumbleLink ? MumbleLink->Context.BuildID : 0);
			if (hunt && lay) { Hunt.Start(AddonFile("echo_hunt.txt"), *lay); }
			else             { Hunt.Stop(); }
		}
		if (Hunt.IsActive()) { ImGui::SameLine(); ImGui::TextDisabled("%s", Hunt.Status().c_str()); }
		ImGui::SetNextItemWidth(120);
		changed |= ImGui::InputInt("Track this species instead (0 = Echo)", &Config.TestSpecies, 0);
		if (Config.TestSpecies < 0) { Config.TestSpecies = 0; }
		ImGui::TextDisabled("Test mode needs a mapped game build and ArcDPS, e.g. Harathi Trampler = 5017.");
		if (ImGui::Button("Open memory diagnostic")) { ShowDiagnostics = true; }
#endif

		if (changed) { SaveSettings(); }
	}

	void AddonLoad(AddonAPI_t* aApi)
	{
		APIDefs = aApi;
		ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
		ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))APIDefs->ImguiMalloc,
		                             (void (*)(void*, void*))APIDefs->ImguiFree);

		NexusLink      = (NexusLinkData_t*)APIDefs->DataLink_Get(DL_NEXUS_LINK);
		MumbleLink     = (Mumble::Data*)APIDefs->DataLink_Get(DL_MUMBLE_LINK);
		MumbleIdentity = (Mumble::Identity*)APIDefs->DataLink_Get(DL_MUMBLE_LINK_IDENTITY);

		std::string dir = APIDefs->Paths_GetAddonDirectory("EndersEcho");
		CreateDirectoryA(dir.c_str(), nullptr);
		SettingsPath = dir + "/settings.ini";
		Config.Load(SettingsPath);
#if !ENDERSECHO_DEV
		Config.TestSpecies = 0; // dev-only settings stay off in the shared build
		Config.AutoMark = false;
#endif
		GameMemory::Init();
		ArcAgents::Init();
#if ENDERSECHO_DEV
		Diagnostics::Init(dir + "/diagnostic.txt");
#endif

		APIDefs->InputBinds_RegisterWithString(KB_TOGGLE_SIM, OnKeybind, "ALT+SHIFT+E");
		APIDefs->InputBinds_RegisterWithString(KB_TOGGLE, OnKeybind, "(null)");

		APIDefs->GUI_Register(RT_Render, AddonRender);
		APIDefs->GUI_Register(RT_OptionsRender, AddonOptions);

		APIDefs->Log(LOGL_INFO, ADDON_NAME, "Loaded.");
	}

	void AddonUnload()
	{
		APIDefs->GUI_Deregister(AddonOptions);
		APIDefs->GUI_Deregister(AddonRender);
		APIDefs->InputBinds_Deregister(KB_TOGGLE_SIM);
		APIDefs->InputBinds_Deregister(KB_TOGGLE);
		// Worker threads must finish before the DLL is unmapped.
		Live.reset();
		LiveChecked = false;
#if ENDERSECHO_DEV
		Hunt.Stop();
		Diagnostics::Shutdown();
#endif
		ArcAgents::Shutdown();
		SaveSettings();

		NexusLink = nullptr;
		MumbleLink = nullptr;
		MumbleIdentity = nullptr;
	}
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
	AddonDef.Signature        = (uint32_t)-0x4E9C1; // unique negative id for a non-Raidcore-hosted addon
	AddonDef.APIVersion       = NEXUS_API_VERSION;
	AddonDef.Name             = ADDON_NAME;
	AddonDef.Version.Major    = 1;
	AddonDef.Version.Minor    = 0;
	AddonDef.Version.Build    = 2;
	AddonDef.Version.Revision = 0;
	AddonDef.Author           = "Lexo";
	AddonDef.Description      = "Dhuum CM: shows where Ender's Echo is, its dash path, and whether you stand in it.";
	AddonDef.Load             = AddonLoad;
	AddonDef.Unload           = AddonUnload;
	AddonDef.Flags            = AF_None;
	// Nexus checks the repository's GitHub releases for new versions.
	AddonDef.Provider         = UP_GitHub;
	AddonDef.UpdateLink       = "https://github.com/Lexo2011/Ender-s-Echo-Helper";
	return &AddonDef;
}
