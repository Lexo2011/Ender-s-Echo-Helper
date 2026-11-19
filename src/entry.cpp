///----------------------------------------------------------------------------------------------------
/// Ender's Echo Helper - Nexus addon for the Dhuum CM encounter.
/// Draws a circle around the player, coloured by how close the player is to Ender's Echo's
/// running path, with an arrow pointing towards the Echo.
///----------------------------------------------------------------------------------------------------

#include "Shared.h"

#include "imgui.h"

#include "ArcAgents.h"
#include "Diagnostics.h"
#include "EchoHunt.h"
#include "GameMemory.h"
#include "LiveEcho.h"
#include "Overlay.h"
#include "Settings.h"
#include "SimulatedEcho.h"

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
	/// Created once the game build is known and has a mapped layout.
	std::unique_ptr<LiveEcho> Live;
	IEchoSource*      LiveSource = nullptr;
	bool              LiveChecked = false;
	EchoHunt          Hunt;

	void EnsureLiveSource()
	{
		if (LiveChecked || !MumbleLink || MumbleLink->Context.BuildID == 0) { return; }
		LiveChecked = true;
		if (const GameMemory::Layout* lay = GameMemory::CurrentLayout(MumbleLink->Context.BuildID))
		{
			Live = std::make_unique<LiveEcho>(*lay);
			Live->SetSpawnFile(std::string(APIDefs->Paths_GetAddonDirectory("EndersEcho")) + "/echo_spawn.txt");
			Live->SetArenaFile(std::string(APIDefs->Paths_GetAddonDirectory("EndersEcho")) + "/echo_arena.txt");
			LiveSource = Live.get();
		}
		else
		{
			APIDefs->Log(LOGL_WARNING, ADDON_NAME, "This game build isn't mapped: live tracking is off. Run the memory diagnostic.");
		}
	}

	/// The Echo exists for a whole attempt, so it is marked once and the flag resets when it disappears.
	/// Pressing the marker bind again on an already marked target would REMOVE the marker.
	bool MarkedThisAttempt = false;

	bool ShowDiagnostics = false;

	Vec3 ToVec3(const Vector3& v) { return { v.X, v.Y, v.Z }; }

	bool IsCommander() { return MumbleIdentity && MumbleIdentity->IsCommander; }

	void PressCrossAgentMarker()
	{
		APIDefs->GameBinds_InvokeAsync(GB_SquadMarkerSetAgent8, 50);
	}

	void UpdateAutoMark(const EchoState& aEcho)
	{
		if (!aEcho.Valid) { MarkedThisAttempt = false; return; }
		if (!Config.AutoMark || MarkedThisAttempt || !LiveSource) { return; }
		if (Config.AutoMarkOnlyAsCommander && !IsCommander()) { return; }

		if (LiveSource->CanMarkDirectly())
		{
			// No targeting needed: the game marks the Echo agent itself.
			if (!LiveSource->MarkEchoDirectly()) { return; }
		}
		else if (LiveSource->IsEchoTargeted())
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

	void SaveSettings() { if (!SettingsPath.empty()) { Config.Save(SettingsPath); } }

	void OnKeybind(const char* aIdentifier, bool aIsRelease)
	{
		if (aIsRelease) { return; }

		if (strcmp(aIdentifier, KB_TOGGLE_SIM) == 0)
		{
			if (Simulation.IsActive())
			{
				Simulation.Stop();
				APIDefs->GUI_SendAlert("Ender's Echo simulation stopped");
			}
			else if (MumbleLink)
			{
				Simulation.Start(ToVec3(MumbleLink->AvatarPosition));
				APIDefs->GUI_SendAlert("Ender's Echo simulation started");
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
		Diagnostics::RenderWindow(&ShowDiagnostics, player);
		if (Hunt.IsActive())
		{
			Hunt.Feed(player, MumbleLink->Context.IsInCombat != 0, MumbleLink->Context.MapID,
			          GetTickCount64() / 1000.0);
		}

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
				const bool testing = Config.TestSpecies > 0;
				if (!testing && Config.OnlyInHallOfChains && MumbleLink->Context.MapID != MAP_HALL_OF_CHAINS) { return; }
				Live->SetSpecies(testing ? (uint32_t)Config.TestSpecies : SPECIES_ENDERS_ECHO);
				echo = Live->Update(player, dt);
				if (!testing) { UpdateAutoMark(echo); }
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
		changed |= ImGui::Checkbox("Show distance (m) next to the arrow", &Config.ShowArrowDistance);
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
			if (Live) { ImGui::TextDisabled("Arena edge learned in %d of 36 directions (the line is 50 m until then).", Live->ArenaKnownDirections()); }
		}

		ImGui::Separator();
		ImGui::Text("Squad marker");
		changed |= ImGui::Checkbox("Auto-mark the Echo with X", &Config.AutoMark);
		if (Config.AutoMark)
		{
			ImGui::Indent();
			changed |= ImGui::Checkbox("Only while I am commander", &Config.AutoMarkOnlyAsCommander);
			ImGui::TextDisabled("Marks the Echo once per attempt. Don't also mark it by hand:");
			ImGui::TextDisabled("placing the same marker again removes it.");
			const bool direct = LiveSource && LiveSource->CanMarkDirectly();
			if (!LiveSource)
			{
				ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Inactive until live tracking of the Echo is implemented.");
			}
			else if (direct)
			{
				ImGui::TextDisabled("Marks directly, no need to target the Echo.");
			}
			else if (LiveSource->CanDetectTarget())
			{
				ImGui::TextDisabled("Marks the moment you target the Echo.");
			}
			if (!direct && !APIDefs->GameBinds_IsBound(GB_SquadMarkerSetAgent8))
			{
				ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1),
					"\"Set Agent Marker: Cross\" has no key. Bind it in game and in Nexus' game keybinds.");
			}
			if (ImGui::Button("Test: mark my current target with X"))
			{
				PressCrossAgentMarker();
			}
			ImGui::Unindent();
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
		ImGui::Text("Testing");
		if (ImGui::Button(Simulation.IsActive() ? "Stop simulation" : "Start simulation"))
		{
			OnKeybind(KB_TOGGLE_SIM, false);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Spawns a fake Echo on a %.0fm arena around you.", Simulation.ArenaRadius);
		if (Live)
		{
			ImGui::Text("Live tracking: %s", Live->Status());
			bool hunt = Hunt.IsActive();
			if (ImGui::Checkbox("Find the Echo's moving position (Dhuum)", &hunt))
			{
				const GameMemory::Layout* lay = GameMemory::CurrentLayout(MumbleLink ? MumbleLink->Context.BuildID : 0);
				if (hunt && lay) { Hunt.Start(std::string(APIDefs->Paths_GetAddonDirectory("EndersEcho")) + "/echo_hunt.txt", *lay); }
				else             { Hunt.Stop(); }
			}
			if (Hunt.IsActive())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%s", Hunt.Status().c_str());
			}
		}
		else
		{
			ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Live tracking is off: this game build isn't mapped.");
		}
		ImGui::SetNextItemWidth(120);
		changed |= ImGui::InputInt("Track this species instead (0 = Echo)", &Config.TestSpecies, 0);
		if (Config.TestSpecies < 0) { Config.TestSpecies = 0; }
		ImGui::TextDisabled("For testing in open world, e.g. Harathi Trampler = 5017. ArcDPS must see it first.");
		if (ImGui::Button("Open memory diagnostic")) { ShowDiagnostics = true; }

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
		GameMemory::Init();
		ArcAgents::Init();
		Diagnostics::Init(dir + "/diagnostic.txt");

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
		LiveSource = nullptr;
		Live.reset();
		Hunt.Stop();
		LiveChecked = false;
		Diagnostics::Shutdown();
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
	AddonDef.Version.Major    = 0;
	AddonDef.Version.Minor    = 1;
	AddonDef.Version.Build    = 0;
	AddonDef.Version.Revision = 0;
	AddonDef.Author           = "Kevin";
	AddonDef.Description      = "Dhuum CM: shows where Ender's Echo is and whether you stand in its path.";
	AddonDef.Load             = AddonLoad;
	AddonDef.Unload           = AddonUnload;
	AddonDef.Flags            = AF_None;
	return &AddonDef;
}
