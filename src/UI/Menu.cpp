#include "UI/Menu.h"

#include "Portal/DoorPair.h"
#include "Portal/DoorwaySnapshot.h"
#include "Portal/PortalManager.h"
#include "Portal/PortalRender.h"
#include "Render/Backend.h"

#include "SKSEMenuFramework.h"

namespace
{
	// The door pair of the doorway the player is at, for the correction controls.
	struct Pair
	{
		RE::TESObjectREFR* source = nullptr;
		RE::TESObjectREFR* linked = nullptr;
		RE::TESObjectREFR* key = nullptr;  // the door the correction is stored under
	};

	// The nearest door that could be a portal, within this. A door switched off here never opens a
	// portal again, so this is the only way back to its settings.
	constexpr float kNearDoorDistance = 600.0f;

	Pair MakePair(RE::FormID a_source, RE::FormID a_linked)
	{
		Pair pair;
		pair.source = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_source);
		pair.linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_linked);
		if (pair.source && pair.linked) {
			pair.key = Portal::Doors::CorrectionDoor(pair.source, pair.linked);
		}
		return pair;
	}

	Pair CurrentPair()
	{
		if (const auto at = Portal::Manager::DoorPairAt(kNearDoorDistance)) {
			return MakePair(at->sourceDoor, at->linkedDoor);
		}
		return {};
	}

	std::string Describe(RE::TESObjectREFR* a_door)
	{
		auto* cell = a_door ? a_door->GetParentCell() : nullptr;
		const char* name = cell && cell->GetFullName() && *cell->GetFullName() ? cell->GetFullName() : (cell ? cell->GetFormEditorID() : "");
		return a_door ? std::format("{:08X} ({})", a_door->GetFormID(), name ? name : "") : "-";
	}

	void __stdcall RenderSettings()
	{
		bool enabled = Portal::Manager::Enabled();
		const bool available = Portal::Manager::HooksInstalled();
		ImGui::BeginDisabled(!available);
		if (ImGui::Checkbox("Seamless doors", &enabled)) {
			Portal::Manager::SetEnabled(enabled);
		}
		ImGui::EndDisabled();
		if (!available) {
			ImGui::TextWrapped("Off for this session: set bEnabled=1 under [Portals] in SeamlessSkyrim.ini and restart the game.");
		}
		ImGui::NewLine();
		ImGui::TextWrapped("Rendering: %s", Render::Backend::Name());
		if (available && !Render::Backend::CanMerge()) {
			ImGui::TextWrapped("Rooms aren't drawn into the world with this renderer (see [Render] sBackend in SeamlessSkyrim.ini).");
		}
		bool merge = Portal::Manager::MergeEnabled();
		if (ImGui::Checkbox("Draw rooms into the world (seen from outside)", &merge)) {
			Portal::Manager::SetMergeEnabled(merge);
		}
		ImGui::NewLine();
		float light = Portal::Manager::MergedLightScale() * 100.0f;
		if (ImGui::SliderFloat("Room light seen from outside (%)", &light, 5.0f, 100.0f, "%.0f")) {
			Portal::Manager::SetMergedLightScale(light / 100.0f);
		}
		ImGui::NewLine();
		float doorDistance = Portal::Manager::DoorOpenDistance();
		if (ImGui::SliderFloat("Doors open within (game units)", &doorDistance, 150.0f, 1000.0f, "%.0f")) {
			Portal::Manager::SetDoorOpenDistance(doorDistance);
		}
		ImGui::TextWrapped("The space behind a door starts loading 300 units further out and is let go 700 units further out.");
		ImGui::NewLine();
		int shown = Portal::Manager::ShownRooms();
		if (ImGui::SliderInt("Rooms active at once", &shown, 1, Portal::Manager::kMaxShownRooms)) {
			Portal::Manager::SetShownRooms(shown);
		}
		ImGui::TextWrapped("Outdoors, the rooms behind the nearest doors are loaded, so a doorway opens at once when you walk up, "
						   "and each open door shows its own room. Counts the room you're at; 1 = only that one. Each costs "
						   "memory, a short frame when it loads, frame time while its doorway is on screen, and its people "
						   "are simulated.");
		if (Render::Backend::MergedRooms() < 2) {
			ImGui::TextWrapped("This renderer draws one room at a time: the others stay loaded out of sight, and their open doors "
							   "show a picture.");
		}
		ImGui::NewLine();
		bool pause = Portal::Manager::PauseUnseenRooms();
		if (ImGui::Checkbox("Pause rooms nobody can see", &pause)) {
			Portal::Manager::SetPauseUnseenRooms(pause);
		}
		ImGui::TextWrapped("A loaded room whose doorway is out of view isn't simulated or drawn until it comes into view; "
						   "the room at the door you're at always runs.");
		ImGui::NewLine();
		{
			// Presets rather than a slider: each size is a texture and a file per door, so a few
			// steps that double are all that's worth offering.
			constexpr int         heights[]{ 64, 128, 192, 256, 384, 512 };
			// The sizes on disk are a door into a room's: its picture reaches past the doorway (3.5
			// times as wide, 1.75 times as high) at twice the density (Snapshot's kCameraExtent and
			// kCameraDensity).
			constexpr const char* labels[]{ "48 x 64 (0.6 MB per door)", "96 x 128 (2.4 MB per door, default)", "144 x 192 (5.4 MB per door)",
				"192 x 256 (9.6 MB per door)", "288 x 384 (22 MB per door)", "384 x 512 (39 MB per door)" };
			const int current = Portal::Snapshot::PictureHeight();
			// An ini value between the presets shows as the nearest one.
			int index = 0;
			for (int i = 0; i < static_cast<int>(std::size(heights)); ++i) {
				if (std::abs(heights[i] - current) < std::abs(heights[index] - current)) {
					index = i;
				}
			}
			if (ImGui::Combo("Doorway picture size (texels)", &index, labels, static_cast<int>(std::size(labels)))) {
				Portal::Snapshot::SetPictureHeight(heights[index]);
			}
			ImGui::TextWrapped("The blurred picture a doorway shows while the space behind it loads, in texels across the doorway. "
							   "Larger is sharper; each door is taken again at the new size when next seen open (doors to the outdoors "
							   "keep one per part of the day, of the doorway only: a 25th of the size).");
		}
		ImGui::NewLine();
		bool step = Portal::Manager::StepExterior();
		if (ImGui::Checkbox("Street physics while indoors (people outside walk)", &step)) {
			Portal::Manager::SetStepExterior(step);
		}
	}

	void __stdcall RenderDoor()
	{
		const auto pair = CurrentPair();
		if (!pair.key) {
			ImGui::TextWrapped("Walk up to a door: its doorway's settings appear here.");
			return;
		}
		ImGui::Text("Door: %s", Describe(pair.source).c_str());
		ImGui::Text("Leads to: %s", Describe(pair.linked).c_str());
		if (ImGui::Button("Save for this door")) {
			Portal::Doors::SaveCorrection(pair.key->GetFormID());
		}
		ImGui::TextWrapped("Everything below applies at once; Save keeps it for this door (both directions).");
		ImGui::Separator();
		bool portal = !Portal::Doors::PortalOff(pair.key->GetFormID());
		if (ImGui::Checkbox("Seamless door (off: an ordinary door with a loading screen)", &portal)) {
			Portal::Doors::SetPortalOff(pair.key->GetFormID(), !portal);
		}
		bool inward = Portal::Doors::OpensInward(pair.key->GetFormID());
		if (ImGui::Checkbox("Opens inward (off: outward, into the street)", &inward)) {
			Portal::Doors::SetOpensInward(pair.key->GetFormID(), inward);
		}
		bool always = Portal::Doors::AlwaysOpen(pair.key->GetFormID());
		if (ImGui::Checkbox("Always open (not only when you come near)", &always)) {
			Portal::Doors::SetAlwaysOpen(pair.key->GetFormID(), always);
		}
		ImGui::Separator();
		ImGui::TextWrapped("If the room behind the doorway doesn't line up with the outside, turn it here. It follows the slider "
						   "live.");
		float degrees = Portal::Doors::Correction(pair.key->GetFormID());
		if (ImGui::SliderFloat("Turn the inside (degrees)", &degrees, -15.0f, 15.0f, "%.1f")) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), degrees);
		}
		if (ImGui::Button("-0.1")) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), degrees - 0.1f);
		}
		ImGui::SameLine();
		if (ImGui::Button("+0.1")) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), degrees + 0.1f);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset")) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), 0.0f);
		}
		ImGui::Separator();
		ImGui::TextWrapped("Move the inside against the outside (game units; sideways and further in as seen walking in):");
		auto shift = Portal::Doors::Shift(pair.key->GetFormID());
		bool moved = ImGui::SliderFloat("Sideways", &shift.x, -60.0f, 60.0f, "%.1f");
		moved |= ImGui::SliderFloat("Further in", &shift.y, -60.0f, 60.0f, "%.1f");
		moved |= ImGui::SliderFloat("Up", &shift.z, -40.0f, 40.0f, "%.1f");
		if (moved) {
			Portal::Doors::SetShift(pair.key->GetFormID(), shift);
		}
		if (ImGui::Button("Reset position")) {
			Portal::Doors::SetShift(pair.key->GetFormID(), {});
		}
		ImGui::Separator();
		ImGui::TextWrapped("Which end of the door is hinged (only guessed from the mesh):");
		int style = static_cast<int>(Portal::Doors::Style(pair.key->GetFormID()));
		bool changed = ImGui::RadioButton("Swing", &style, 0);
		ImGui::SameLine();
		changed |= ImGui::RadioButton("Swing, other hinge", &style, 1);
		if (changed) {
			Portal::Doors::SetStyle(pair.key->GetFormID(), static_cast<Portal::Doors::OpenStyle>(style));
		}
	}
}

namespace UI::Menu
{
	void Register()
	{
		// The framework's header looks the DLL up while ours loads, which can be before it (plugins
		// load in name order): looked up again now that every plugin is loaded.
		menuFramework = ::GetModuleHandleW(L"SKSEMenuFramework");
		if (!menuFramework) {
			logger::info("menu: SKSE Menu Framework not installed; no in-game settings");
			return;
		}
		SKSEMenuFramework::SetSection("Seamless Skyrim");
		SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
		SKSEMenuFramework::AddSectionItem("This doorway", RenderDoor);
		logger::info("menu: registered with SKSE Menu Framework");
	}
}
