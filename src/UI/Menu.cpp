#include "UI/Menu.h"

#include "Portal/DoorPair.h"
#include "Portal/DoorUse.h"
#include "Portal/DoorwaySnapshot.h"
#include "Portal/PortalManager.h"
#include "Portal/PortalRender.h"
#include "Render/Backend.h"
#include "UI/Translation.h"

#include "Paths.h"
#include "SKSEMenuFramework.h"

namespace
{
	namespace T = UI::Translation;

	// Not TextWrapped(text): a translation with a % in it would be read as a format.
	void Wrapped(std::string_view a_key)
	{
		ImGui::TextWrapped("%s", T::Text(a_key));
	}

	// What is set here is written to SeamlessSkyrim.ini, as the ini's comments say of these keys
	// ("Also in the in-game menu"): kept for the session only, every setting was back at the next
	// start of the game.
	void Save(const wchar_t* a_key, int a_value)
	{
		const auto ini = Paths::PluginDirectory() / "SeamlessSkyrim.ini";
		::WritePrivateProfileStringW(L"Portals", a_key, std::to_wstring(a_value).c_str(), ini.c_str());
	}

	// A slider is written once, when it is let go: dragged, it changes every frame.
	void SaveWhenReleased(const wchar_t* a_key, int a_value)
	{
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			Save(a_key, a_value);
		}
	}

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

	bool Outdoors(RE::TESObjectREFR* a_door)
	{
		auto* cell = a_door->GetParentCell();
		return cell ? !cell->IsInteriorCell() : a_door->GetWorldspace() != nullptr;
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

		ImGui::TextWrapped("%s", T::Format("settings.rendering", Render::Backend::Name()).c_str());
		if (available && !Render::Backend::CanMerge()) {
			Wrapped("settings.no_merge_renderer");
		}

		ImGui::NewLine();

		ImGui::BeginDisabled(!available);
		if (ImGui::Checkbox(T::Label("settings.enabled"), &enabled)) {
			Portal::Manager::SetEnabled(enabled);
		}
		ImGui::EndDisabled();
		if (!available) {
			Wrapped("settings.enabled_unavailable");
		} else {
			// Not written to the ini: off there, the game starts without the mod's hooks, and this
			// box can't switch it on again.
			Wrapped("settings.enabled_session");
		}

		ImGui::NewLine();
		
		bool merge = Portal::Manager::MergeEnabled();
		if (ImGui::Checkbox(T::Label("settings.merge"), &merge)) {
			Portal::Manager::SetMergeEnabled(merge);
			Save(L"bMerge", merge);
		}

		ImGui::NewLine();

		int shown = Portal::Manager::ShownRooms();
		if (ImGui::SliderInt(T::Label("settings.shown_rooms"), &shown, 1, Portal::Manager::kMaxShownRooms)) {
			Portal::Manager::SetShownRooms(shown);
		}
		SaveWhenReleased(L"iShownRooms", shown);
		Wrapped("settings.shown_rooms_help");
		if (Render::Backend::MergedRooms() < 2) {
			Wrapped("settings.shown_rooms_one_at_a_time");
		}

		ImGui::NewLine();

		bool pause = Portal::Manager::PauseUnseenRooms();
		if (ImGui::Checkbox(T::Label("settings.pause_unseen"), &pause)) {
			Portal::Manager::SetPauseUnseenRooms(pause);
			Save(L"bPauseUnseenRooms", pause);
		}
		Wrapped("settings.pause_unseen_help");

		ImGui::NewLine();

		bool step = Portal::Manager::StepExterior();
		if (ImGui::Checkbox(T::Label("settings.step_exterior"), &step)) {
			Portal::Manager::SetStepExterior(step);
			Save(L"bStepExteriorFromInside", step);
		}

		ImGui::NewLine();

		{
			int         force = static_cast<int>(Portal::DoorUse::GetForce());
			const char* labels[]{ T::Text("settings.doors_as_set"), T::Text("settings.doors_all_open"), T::Text("settings.doors_all_shut") };
			if (ImGui::Combo(T::Label("settings.doors"), &force, labels, static_cast<int>(std::size(labels)))) {
				Portal::DoorUse::SetForce(static_cast<Portal::DoorUse::Force>(force));
				Save(L"iForceDoors", force);
			}
			Wrapped("settings.doors_help");
		}

		ImGui::NewLine();

		float doorDistance = Portal::Manager::DoorOpenDistance();
		if (ImGui::SliderFloat(T::Label("settings.door_reach"), &doorDistance, 150.0f, 1000.0f, "%.0f")) {
			Portal::Manager::SetDoorOpenDistance(doorDistance);
		}
		SaveWhenReleased(L"iDoorOpenDistance", static_cast<int>(std::lround(doorDistance)));
		Wrapped("settings.door_reach_help");

		ImGui::NewLine();

		{
			// Presets rather than a slider: each size is a texture and a file per door, so a few
			// steps that double are all that's worth offering.
			constexpr int         heights[]{ 64, 128, 192, 256, 384, 512 };
			// The sizes on disk are a door into a room's: its picture reaches past the doorway (3.5
			// times as wide, 1.75 times as high) at twice the density (Snapshot's kCameraExtent and
			// kCameraDensity).
			constexpr int         widths[]{ 48, 96, 144, 192, 288, 384 };
			constexpr const char* megabytes[]{ "0.6", "2.4", "5.4", "9.6", "22", "39" };
			constexpr std::size_t defaultIndex = 1;
			// The language is set once, before the menu is first drawn.
			static const auto texts = [&] {
				std::array<std::string, std::size(heights)> result;
				for (std::size_t i = 0; i < result.size(); ++i) {
					result[i] = T::Format(i == defaultIndex ? "settings.picture_size_option_default" : "settings.picture_size_option",
						widths[i], heights[i], megabytes[i]);
				}
				return result;
			}();
			const char* labels[std::size(heights)];
			std::ranges::transform(texts, labels, &std::string::c_str);
			const int current = Portal::Snapshot::PictureHeight();
			// An ini value between the presets shows as the nearest one.
			int index = 0;
			for (int i = 0; i < static_cast<int>(std::size(heights)); ++i) {
				if (std::abs(heights[i] - current) < std::abs(heights[index] - current)) {
					index = i;
				}
			}
			if (ImGui::Combo(T::Label("settings.picture_size"), &index, labels, static_cast<int>(std::size(labels)))) {
				Portal::Snapshot::SetPictureHeight(heights[index]);
				Save(L"iDoorwayPictureHeight", heights[index]);
			}
			Wrapped("settings.picture_size_help");
			// Until another click: the result of this one, so the click visibly did something.
			static std::optional<int> deleted;
			if (ImGui::Button(T::Label("settings.delete_pictures"))) {
				deleted = Portal::Snapshot::DeleteAll();
			}
			if (deleted) {
				ImGui::SameLine();
				ImGui::TextUnformatted(T::Format("settings.pictures_deleted", *deleted).c_str());
			}
			Wrapped("settings.delete_pictures_help");
		}
	}

	void __stdcall RenderDoor()
	{
		const auto pair = CurrentPair();
		if (!pair.key) {
			Wrapped("door.none");
			return;
		}
		ImGui::TextUnformatted(T::Format("door.door", Describe(pair.source)).c_str());
		ImGui::TextUnformatted(T::Format("door.leads_to", Describe(pair.linked)).c_str());

		ImGui::NewLine();

		if (ImGui::Button(T::Label("door.save"))) {
			Portal::Doors::SaveCorrection(pair.key->GetFormID());
		}

		ImGui::NewLine();

		bool portal = !Portal::Doors::PortalOff(pair.key->GetFormID());
		if (ImGui::Checkbox(T::Label("door.portal"), &portal)) {
			Portal::Doors::SetPortalOff(pair.key->GetFormID(), !portal);
		}
		bool inward = Portal::Doors::OpensInward(pair.key->GetFormID());
		if (ImGui::Checkbox(T::Label("door.opens_inward"), &inward)) {
			Portal::Doors::SetOpensInward(pair.key->GetFormID(), inward);
		}
		bool always = Portal::Doors::AlwaysOpen(pair.key->GetFormID());
		if (ImGui::Checkbox(T::Label("door.always_open"), &always)) {
			Portal::Doors::SetAlwaysOpen(pair.key->GetFormID(), always);
		}
		if (Outdoors(pair.source) && Outdoors(pair.linked)) {
			bool picture = Portal::Doors::PictureFromAfar(pair.key->GetFormID());
			if (ImGui::Checkbox(T::Label("door.picture_from_afar"), &picture)) {
				Portal::Doors::SetPictureFromAfar(pair.key->GetFormID(), picture);
			}
		}
		
		ImGui::NewLine();

		Wrapped("door.turn_help");
		float degrees = Portal::Doors::Correction(pair.key->GetFormID());
		if (ImGui::SliderFloat(T::Label("door.turn"), &degrees, -15.0f, 15.0f, "%.1f")) {
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
		if (ImGui::Button(T::Label("door.turn_reset"))) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), 0.0f);
		}

		ImGui::NewLine();

		Wrapped("door.shift_help");
		auto shift = Portal::Doors::Shift(pair.key->GetFormID());
		bool moved = ImGui::SliderFloat(T::Label("door.sideways"), &shift.x, -60.0f, 60.0f, "%.1f");
		moved |= ImGui::SliderFloat(T::Label("door.further_in"), &shift.y, -60.0f, 60.0f, "%.1f");
		moved |= ImGui::SliderFloat(T::Label("door.up"), &shift.z, -40.0f, 40.0f, "%.1f");
		if (moved) {
			Portal::Doors::SetShift(pair.key->GetFormID(), shift);
		}
		if (ImGui::Button(T::Label("door.shift_reset"))) {
			Portal::Doors::SetShift(pair.key->GetFormID(), {});
		}
		
		ImGui::NewLine();

		float swing = Portal::Doors::SwingDegrees(pair.key->GetFormID());
		if (ImGui::SliderFloat(T::Label("door.swing"), &swing, 30.0f, 120.0f, "%.0f")) {
			Portal::Doors::SetSwingDegrees(pair.key->GetFormID(), swing);
		}
		ImGui::SameLine();
		if (ImGui::Button(T::Label("door.swing_reset"))) {
			Portal::Doors::SetSwingDegrees(pair.key->GetFormID(), Portal::Doors::kSwingDegrees);
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
		T::Load();
		// The mod's name stays as it is in every language.
		SKSEMenuFramework::SetSection("Seamless Skyrim");
		SKSEMenuFramework::AddSectionItem(T::Text("settings.section"), RenderSettings);
		SKSEMenuFramework::AddSectionItem(T::Text("door.section"), RenderDoor);
		logger::info("menu: registered with SKSE Menu Framework");
	}
}
