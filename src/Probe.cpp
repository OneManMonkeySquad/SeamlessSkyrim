#include "Probe.h"

namespace
{
	using CellState = RE::TESObjectCELL::CellState;

	std::string_view StateName(CellState a_state)
	{
		switch (a_state) {
		case CellState::kUnloaded:   return "Unloaded"sv;
		case CellState::kUnloading:  return "Unloading"sv;
		case CellState::kDataLoaded: return "DataLoaded"sv;
		case CellState::kBuilding3D: return "Building3D"sv;
		case CellState::kUnattached: return "Unattached"sv;
		case CellState::kDetaching:  return "Detaching"sv;
		case CellState::kAttaching:  return "Attaching"sv;
		case CellState::kAttached:   return "Attached"sv;
		default:                     return "?"sv;
		}
	}

	struct ActorCounts
	{
		int total = 0;
		int high = 0;
		int with3D = 0;
	};

	ActorCounts CountActors(const RE::TESObjectCELL* a_cell)
	{
		ActorCounts counts;
		a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			if (auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr) {
				++counts.total;
				auto* process = actor->GetActorRuntimeData().currentProcess;
				if (process && process->InHighProcess()) {
					++counts.high;
				}
				if (actor->Get3D()) {
					++counts.with3D;
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return counts;
	}

	std::string Describe(RE::TESObjectCELL* a_cell)
	{
		if (!a_cell) {
			return "<none>";
		}
		const char* edid = a_cell->GetFormEditorID();
		const auto* loaded = a_cell->GetRuntimeData().loadedData;
		const auto  actors = CountActors(a_cell);
		return std::format(
			"{:08X} '{}' state={} 3D={} actors={} (high={}, with3D={})",
			a_cell->GetFormID(),
			edid && *edid ? edid : a_cell->GetFullName(),
			StateName(a_cell->cellState.get()),
			loaded && loaded->cell3D ? "yes" : "no",
			actors.total, actors.high, actors.with3D);
	}

	class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent>
	{
	public:
		static ActivateSink* GetSingleton()
		{
			static ActivateSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
		{
			if (!a_event || !a_event->objectActivated || a_event->actionRef.get() != RE::PlayerCharacter::GetSingleton()) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto* door = a_event->objectActivated.get();
			auto* teleport = door->extraList.GetByType<RE::ExtraTeleport>();
			if (!teleport || !teleport->teleportData) {
				return RE::BSEventNotifyControl::kContinue;
			}

			// Logged before the load screen runs, so the target cell's state here tells us whether
			// it was still resident from an earlier visit.
			const auto& data = *teleport->teleportData;
			auto        linked = data.linkedDoor.get();
			const auto  pos = door->GetPosition();
			logger::info("--- load door {:08X} at ({:.0f}, {:.0f}, {:.0f}) in {}",
				door->GetFormID(), pos.x, pos.y, pos.z, Describe(door->GetParentCell()));
			logger::info("    -> linked door {:08X}, arrival ({:.0f}, {:.0f}, {:.0f}) rotZ={:.3f}",
				linked ? linked->GetFormID() : 0u,
				data.position.x, data.position.y, data.position.z, data.rotation.z);
			if (linked && !linked->GetParentCell()) {
				// Seen for interior -> exterior doors: the linked door is a persistent ref whose
				// exterior cell isn't loaded, so it has no parent cell yet.
				logger::info("    -> target: unloaded exterior (worldspace {:08X})",
					linked->GetWorldspace() ? linked->GetWorldspace()->GetFormID() : 0u);
			} else {
				logger::info("    -> target cell {}", Describe(linked ? linked->GetParentCell() : nullptr));
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	class CellLoadedSink final : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
	{
	public:
		static CellLoadedSink* GetSingleton()
		{
			static CellLoadedSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* a_event, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
		{
			// Exterior grids fire this once per cell; only interiors mark a door transition.
			if (a_event && a_event->cell && a_event->cell->IsInteriorCell()) {
				Probe::DumpWorldState(std::format("interior fully loaded: {:08X}", a_event->cell->GetFormID()));
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};
}

namespace Probe
{
	std::string DescribeCell(RE::TESObjectCELL* a_cell)
	{
		return Describe(a_cell);
	}

	void Install()
	{
		auto* events = RE::ScriptEventSourceHolder::GetSingleton();
		events->AddEventSink<RE::TESActivateEvent>(ActivateSink::GetSingleton());
		events->AddEventSink<RE::TESCellFullyLoadedEvent>(CellLoadedSink::GetSingleton());
		logger::info("probe installed");
	}

	void DumpWorldState(std::string_view a_reason)
	{
		logger::info("=== world state ({})", a_reason);

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* tes = RE::TES::GetSingleton();
		logger::info("player cell:   {}", Describe(player ? player->GetParentCell() : nullptr));
		logger::info("TES interior:  {}", Describe(tes ? tes->interiorCell : nullptr));

		// Every interior that is not fully unloaded. This is the cell buffer we would like to
		// exploit: if a cell we just left stays here as Unattached with its 3D, the portal can
		// reuse it instead of loading a second copy.
		if (auto* data = RE::TESDataHandler::GetSingleton()) {
			auto* playerCell = player ? player->GetParentCell() : nullptr;
			int   resident = 0;
			for (auto* cell : data->interiorCells) {
				if (cell && cell->cellState.get() != CellState::kUnloaded && cell != playerCell) {
					logger::info("resident:      {}", Describe(cell));
					++resident;
				}
			}
			logger::info("{} other interior(s) resident", resident);
		}

		// Which cells the engine is running full AI/animation for. Actors in a non-current
		// interior showing up here would mean dual-cell simulation is closer than expected.
		if (auto* processes = RE::ProcessLists::GetSingleton()) {
			std::unordered_map<RE::TESObjectCELL*, int> highPerCell;
			for (auto& handle : processes->highActorHandles) {
				if (auto actor = handle.get()) {
					++highPerCell[actor->GetParentCell()];
				}
			}
			for (auto& [cell, count] : highPerCell) {
				logger::info("high-process:  {} actor(s) in {:08X}", count, cell ? cell->GetFormID() : 0u);
			}
		}
	}
}
