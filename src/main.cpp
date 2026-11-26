#include "Bridge/AgentMode.h"
#include "Bridge/Bridge.h"
#include "Bridge/FrameHooks.h"
#include "Portal/NpcTravel.h"
#include "Portal/PortalManager.h"
#include "Portal/Travel.h"
#include "UI/Menu.h"
#include "Probe.h"

// Runs on SE 1.5.97 and on AE from 1.6.353, on both of AE's struct layouts (before and after
// 1.6.629): CommonLib picks the layout at runtime. SKSE's declaration can only name one layout, and
// SKSE 2.1.5 (for 1.6.353) refuses the flag that names the later one, so we declare no struct use,
// as Community Shaders does, and turn away the AE builds before 1.6.353 ourselves in
// SKSEPluginLoad. SE's SKSE ignores this declaration and loads us through SKSEPlugin_Query.
SKSEPluginInfo(
	.Version = REL::Version{ PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH, 0 },
	.Name = PLUGIN_NAME,
	.Author = "bloodline",
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::PluginDeclaration::RuntimeCompatibility(SKSE::VersionIndependence::AddressLibrary))

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			// Event sources, the data handler and the renderer's swap chain all exist by now.
			Portal::Manager::Install();
			if (Portal::Manager::HooksInstalled()) {
				Portal::NpcTravel::Install();
			}
			UI::Menu::Register();
			FrameHooks::Install();
			if (AgentMode::Enabled()) {
				Bridge::ForceAlwaysActive();
				AgentMode::InstallInputGuards();
				AgentMode::InstallAutoLoad();
			}
			break;
		// A foreign cell attached under our root must never end up in a save, and a load replaces
		// the world the portal refers to; the manager re-attaches on the next frames anyway.
		case SKSE::MessagingInterface::kSaveGame:
			// The save is written over the next frames; re-attaching right away (seen 0.1 s later)
			// would put the cell back while it's being serialized.
			// Except in a room walked into: let go, it would leave the player standing in the walls
			// of its house, and fall (see Manager::NoteRoomForSave).
			if (!Portal::Manager::NoteRoomForSave()) {
				Portal::Manager::ReleaseAll("saving", 3.0f);
				Portal::Travel::CloseArrivalDoor();
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			Portal::Manager::ReleaseAll("loading a save", 3.0f);
			Portal::Manager::ForgetDoors();
			Portal::Travel::ForgetTrip();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Probe::DumpWorldState("game loaded");
			Portal::Manager::EnterRoomAfterLoad();
			break;
		case SKSE::MessagingInterface::kNewGame:
			Probe::DumpWorldState("game loaded");
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	// Every line written through: a game killed on exit (a crash in another mod's shutdown) lost
	// the end of the log, which was where a test's findings were.
	spdlog::default_logger()->flush_on(spdlog::level::trace);
	// The first AE builds were never checked against our hook sites and struct offsets.
	if (REL::Module::IsAE() && REL::Module::get().version() < SKSE::RUNTIME_SSE_1_6_353) {
		logger::error("{} needs Skyrim 1.5.97, or 1.6.353 or later; this is {}", SKSE::GetPluginName(), REL::Module::get().version().string());
		return false;
	}
	SKSE::AllocTrampoline(2048);
	if (auto* serialization = SKSE::GetSerializationInterface()) {
		serialization->SetUniqueID('SMLS');
		serialization->SetSaveCallback(Portal::Manager::SaveRoom);
		serialization->SetLoadCallback(Portal::Manager::LoadRoom);
		serialization->SetRevertCallback([](SKSE::SerializationInterface*) { Portal::Manager::ForgetRoom(); });
	}
	logger::info("{} {} loaded", SKSE::GetPluginName(), SKSE::GetPluginVersion().string());

	// Before anything else touches windows: the game window doesn't exist yet at this point.
	AgentMode::InstallFocusGuards();
	AgentMode::InstallMute();
	SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);
	// Agent sessions only (the MCP server sets SEAMLESS_AGENT): normal play gets no bridge and
	// keeps the game's own alt-tab behaviour.
	if (AgentMode::Enabled()) {
		// Started early so the MCP server can tell "game is starting" from "game not running".
		Bridge::Start();
		// Also forced again at kDataLoaded; this early call covers startup when the agent has
		// already taken focus away from the game.
		Bridge::ForceAlwaysActive();
	}
	logger::info("agent mode: {}", AgentMode::Enabled() ? (AgentMode::Background() ? "on (background)" : "on") : "off");
	return true;
}
