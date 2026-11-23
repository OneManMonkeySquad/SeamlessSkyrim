#include "Portal/NpcTravel.h"

#include "Portal/DoorPair.h"
#include "Portal/DoorUse.h"

#include "Hooks.h"

namespace
{
	// An NPC whose path reaches a door first plays the activate action aimed at it (stops, reaches
	// for the handle) and opens the door only when that animation says so; when no such idle starts,
	// the engine opens the door at once. Through a doorway standing open there is no handle to reach
	// for, so no idle starts; a shut one opens as the NPC reaches for it (DoorUse), as a door does.
	struct DoorIdleHook
	{
		static bool Thunk(RE::AIProcess* a_process, RE::Actor* a_actor, std::uint32_t a_action, RE::TESBoundObject* a_object, RE::TESObjectREFR* a_door)
		{
			if (a_door && Portal::DoorUse::StandsOpen(a_door->GetFormID())) {
				logger::info("portal: {} goes through the open door {:08X} without reaching for it", a_actor ? a_actor->GetDisplayFullName() : "?", a_door->GetFormID());
				return false;
			}
			if (a_door) {
				Portal::DoorUse::NoteActorUse(a_door->GetFormID());
			}
			return func(a_process, a_actor, a_action, a_object, a_door);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	struct Seen
	{
		bool         remote;  // was in the space behind the door
		RE::NiPoint3 position;
		float        heading;
	};

	std::unordered_map<RE::FormID, Seen> lastFrame;
	// The door pair lastFrame's sides are told by. After the player's own crossing the pair is the
	// other way round and everyone has "changed sides": only the two spaces' coordinates lying far
	// apart kept them from being carried through the door.
	std::uint64_t                        lastFramePortal = 0;
	std::uint32_t                        carried = 0;
	std::string                          lastCarried = "none";

	// Only NPCs that were at the door count: anything else that changes space (a script moving
	// someone, a follower catching up) is none of our business.
	constexpr float kDoorRange = 200.0f;

	// An NPC using a load door stops a good metre in front of it, reaches for the door, and is put
	// at the marker behind it: in an open doorway that is a pause and then a jump of a metre and
	// a half. After the engine's transition the NPC is put back where it stood (as seen through
	// the doorway) and walked the rest of the way by us, in a straight line through the doorway.
	struct Walk
	{
		RE::ActorHandle actor;
		RE::NiPoint3    from, to;
		float           heading;
		float           gravity;
		std::chrono::steady_clock::time_point began;
		bool            wentIn = false;
		float           crossAt = 0.0f;  // fraction of the way at which the door plane is passed
		bool            crossed = false;
		bool            driven = false;  // moved by us, because it didn't walk by itself
	};
	std::vector<Walk> walks;

	// The engine fades an NPC out in front of a load door (half a second, standing still), moves
	// it when it has gone, and fades it in again behind the door: with the doorway open, someone
	// dissolving on the porch and forming again inside. The fade-out is cut short (the move then
	// follows at once), the fade-in is cancelled, and around their passage these actors are drawn
	// fully opaque whatever the engine's fade says.
	std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> keptVisible;

	void KeepVisible(RE::Actor* a_actor)
	{
		keptVisible[a_actor->GetFormID()] = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
	}

	void UpdateVisible()
	{
		const auto now = std::chrono::steady_clock::now();
		std::erase_if(keptVisible, [&](const auto& a_entry) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_entry.first);
			auto* object = actor ? actor->Get3D() : nullptr;
			if (now > a_entry.second) {
				return true;
			}
			if (!object) {
				return false;
			}
			auto* process = actor->GetActorRuntimeData().currentProcess;
			if (auto* high = process ? process->high : nullptr; high && (high->fadeState == RE::HighProcessData::FADE_STATE::kTeleportIn || high->fadeState == RE::HighProcessData::FADE_STATE::kIn)) {
				high->fadeState = RE::HighProcessData::FADE_STATE::kNormal;
				high->fadeAlpha = 1.0f;
			}
			actor->SetAlpha(1.0f);
			RE::BSVisit::TraverseScenegraphObjects(object, [](RE::NiAVObject* a_object) {
				if (auto* fade = a_object->AsFadeNode()) {
					fade->GetRuntimeData().currentFade = 1.0f;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return false;
		});
	}
	constexpr float   kWalkSpeed = 90.0f;

	void EndWalk(Walk& a_walk, RE::Actor* a_actor)
	{
		if (a_actor) {
			a_actor->NotifyAnimationGraph("moveStop");
		}
		if (auto* controller = a_actor ? a_actor->GetCharController() : nullptr) {
			controller->gravity = a_walk.gravity;
		}
	}

	void UpdateWalks()
	{
		std::erase_if(walks, [](Walk& a_walk) {
			auto        actor = a_walk.actor.get();
			const auto  path = a_walk.to - a_walk.from;
			const float distance = std::max(path.Length(), 1.0f);
			const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - a_walk.began).count();
			float       done = 1.0f;
			if (actor && !actor->IsDead() && actor->Get3D()) {
				// Left to itself the NPC doesn't walk this stretch (measured: 0% of the way in half a
				// second; there is no navmesh short of a doorway, and coming out it stands inside
				// the building's collision), so it is moved, at walking pace.
				a_walk.driven = true;
				done = elapsed * kWalkSpeed / distance;
			}
			if (done >= 1.0f) {
				Portal::Manager::RefreshMergedShapes();
				if (actor && a_walk.driven) {
					actor->SetPosition(a_walk.to, true);
				}
				EndWalk(a_walk, actor.get());
				return true;
			}
			if (!a_walk.crossed && done >= a_walk.crossAt) {
				a_walk.crossed = true;
				Portal::Manager::RefreshMergedShapes();
			}
			actor->SetPosition(a_walk.from + path * done, true);
			actor->SetHeading(a_walk.heading);
			actor->SetGraphVariableFloat("Speed", kWalkSpeed);
			actor->SetGraphVariableFloat("Direction", 0.0f);
			// Short of the doorway there is no floor on the new side.
			if (auto* controller = actor->GetCharController()) {
				controller->gravity = 0.0f;
				controller->fallTime = 0.0f;
				RE::hkVector4 velocity;
				controller->GetLinearVelocityImpl(velocity);
				if (velocity.quad.m128_f32[2] < 0.0f) {
					velocity.quad.m128_f32[2] = 0.0f;
					controller->SetLinearVelocityImpl(velocity);
				}
			}
			return false;
		});
	}
}

namespace Portal::NpcTravel
{
	void Install()
	{
		// MovementMessageActivateDoor's handler, its call that starts the activate idle at the door.
		REL::Relocation<std::uintptr_t> site{ REL::RelocationID(36854, 37878), REL::Relocate(0x1A3, 0x1AA) };
		if (Hooks::CallsTo(site.address(), REL::RelocationID(38289, 39255), "npc travel: door idle")) {
			DoorIdleHook::func = SKSE::GetTrampoline().write_call<5>(site.address(), DoorIdleHook::Thunk);
		}
	}

	void Reset()
	{
		UpdateWalks();
		UpdateVisible();
		lastFrame.clear();
	}

	void Update(const Manager::Active& a_portal, const std::vector<RE::TESObjectCELL*>& a_remoteCells)
	{
		const auto found = Doors::TryCompute(a_portal);
		if (!found) {
			return;
		}
		const auto& mapping = *found;
		const auto doors = a_portal.sourceDoor | (static_cast<std::uint64_t>(a_portal.linkedDoor) << 32);
		if (std::exchange(lastFramePortal, doors) != doors) {
			lastFrame.clear();
		}
		const auto linkedCenter = mapping.toLinked * mapping.sourceCenter;
		const auto toSource = mapping.toLinked.Invert();
		UpdateWalks();

		std::unordered_map<RE::FormID, Seen> now;
		auto*                                 lists = RE::ProcessLists::GetSingleton();
		for (auto& handle : lists->highActorHandles) {
			auto actor = handle.get();
			auto* cell = actor ? actor->GetParentCell() : nullptr;
			if (!cell || actor->IsPlayerRef() || actor->IsDead()) {
				continue;
			}
			const bool remote = std::ranges::find(a_remoteCells, cell) != a_remoteCells.end();
			Seen       seen{ remote, actor->GetPosition(), actor->GetAngleZ() };

			const auto before = lastFrame.find(actor->GetFormID());
			if (before != lastFrame.end() && before->second.remote != remote) {
				// It changed sides since the last frame. Where it was, mapped to the side it is on now.
				const auto& old = before->second;
				const bool  wentIn = !old.remote;  // from the player's space into the remote one
				const auto& door = wentIn ? mapping.sourceCenter : linkedCenter;
				if (old.position.GetDistance(door) < kDoorRange) {
					// DoorIdleHook keeps the usual reach for the door from starting; one started some
					// other way (not every NPC came in through that handler: Jon Battle-Born, following
					// Carlotta in, didn't) would play on through the walk below.
					actor->NotifyAnimationGraph("IdleForceDefaultState");
					// The engine's own transition has already put the actor at the door's marker; it is
					// moved back to where it would be had it simply walked on, so it doesn't jump in
					// the picture. But not short of the doorway on the new side: there is no floor and
					// no navmesh behind a door (Embry, carried into the inn from the porch, sank 37
					// units and never moved again). So at least a step past the door plane, at the
					// height the marker gave.
					constexpr float kPastDoor = 40.0f;
					const float     markerZ = actor->GetPositionZ();
					auto            point = wentIn ? old.position : toSource * old.position;
					const auto      offset = point - mapping.sourceCenter;
					const float     depth = offset.x * mapping.inward.x + offset.y * mapping.inward.y;  // > 0: remote side
					if (wentIn && depth < kPastDoor) {
						point += mapping.inward * (kPastDoor - depth);
					} else if (!wentIn && depth > -kPastDoor) {
						point -= mapping.inward * (depth + kPastDoor);
					}
					auto start = wentIn ? mapping.toLinked * old.position : toSource * old.position;
					seen.position = wentIn ? mapping.toLinked * point : point;
					if (std::abs(seen.position.z - markerZ) < 100.0f) {
						seen.position.z = markerZ;
						start.z = markerZ;
					}
					seen.heading = std::atan2(seen.position.x - start.x, seen.position.y - start.y);
					if (start.GetDistance(seen.position) > 10.0f && std::ranges::none_of(walks, [&](const Walk& a_walk) { return a_walk.actor.get().get() == actor.get(); })) {
						auto* controller = actor->GetCharController();
						// How far in front of the plane it starts (depth is that of the unclamped point);
						// already past it, it is on the new side from the first step.
						const float before = std::max(wentIn ? -depth : depth, 0.0f);
						walks.push_back({ actor->GetHandle(), start, seen.position, seen.heading, controller ? controller->gravity : 1.0f, std::chrono::steady_clock::now(), wentIn,
							before / (before + kPastDoor) });
						actor->SetPosition(start, true);
						actor->NotifyAnimationGraph("moveStart");
						seen.position = start;
					} else {
						seen.heading = old.heading + (wentIn ? mapping.yawDelta : -mapping.yawDelta);
						actor->SetPosition(seen.position, true);
					}
					actor->SetHeading(seen.heading);
					if (wentIn) {
						actor->MoveToHigh();  // the engine demotes NPCs entering a cell that isn't the player's
					}
					// The engine places a moved actor's 3D at its next update; until then the 3D, already
					// under the new space's node, kept the old space's coordinates: drawn thousands of
					// units off for a frame (seen as a head alone at the door), and coming out, hidden by
					// the engine's actor cull for one more frame over its stale bounds.
					actor->Update3DPosition(true);
					if (auto* root = actor->Get3D()) {
						RE::NiUpdateData update;
						root->Update(update);
					}
					KeepVisible(actor.get());
					Manager::RefreshMergedShapes();
					++carried;
					lastCarried = std::format("{} {} at ({:.0f},{:.0f},{:.0f})", actor->GetDisplayFullName(), wentIn ? "in" : "out", seen.position.x, seen.position.y, seen.position.z);
					logger::info("portal: carried {} through the door", lastCarried);
				}
			}
			now.emplace(actor->GetFormID(), seen);
		}
		lastFrame = std::move(now);
		UpdateVisible();
	}

	// Cells whose references the engine fades in, shown at once until then (ShowAtOnce): the one
	// the player just walked into, the street just loaded behind a door.
	std::vector<std::pair<RE::TESObjectCELL*, std::chrono::steady_clock::time_point>> shownCells;

	// No NPC fades at a door, the portal's or any other: with no loading screen anywhere, an NPC
	// dissolving at a door and forming again behind it looked like a ghost. The fade-out is cut short
	// (the engine then moves them at once), they stay opaque meanwhile, and the fade-in is skipped.
	void NoFades()
	{
		for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
			auto  actor = handle.get();
			auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
			auto* high = process ? process->high : nullptr;
			if (!high || actor->IsPlayerRef()) {
				continue;
			}
			using Fade = RE::HighProcessData::FADE_STATE;
			if (high->fadeState == Fade::kTeleportOut) {
				high->fadeAlpha = std::min(high->fadeAlpha, 0.01f);
				KeepVisible(actor.get());
			} else if (high->fadeState == Fade::kTeleportIn || (high->fadeState == Fade::kIn && keptVisible.contains(actor->GetFormID()))) {
				// The plain fade-in only as the end of a door's (seen fading out, or carried through):
				// it is also how a script or a summoning lets someone appear, anywhere.
				KeepVisible(actor.get());
			}
		}
		const auto now = std::chrono::steady_clock::now();
		std::erase_if(shownCells, [&](const auto& a_shown) { return now >= a_shown.second; });
		for (auto& [cell, until] : shownCells) {
			auto* loaded = cell->GetRuntimeData().loadedData;
			if (auto* cell3D = loaded ? loaded->cell3D.get() : nullptr) {
				RE::BSVisit::TraverseScenegraphObjects(cell3D, [](RE::NiAVObject* a_object) {
					if (auto* fade = a_object->AsFadeNode()) {
						fade->GetRuntimeData().currentFade = 1.0f;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}
	}

	void ShowAtOnce(RE::TESObjectCELL* a_cell, float a_seconds)
	{
		if (!a_cell) {
			return;
		}
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(a_seconds * 1000.0f));
		const auto it = std::ranges::find(shownCells, a_cell, &decltype(shownCells)::value_type::first);
		if (it == shownCells.end()) {
			shownCells.emplace_back(a_cell, until);
		} else {
			it->second = std::max(it->second, until);
		}
	}

	void BeforeRender()
	{
		NoFades();
		UpdateVisible();
	}

	void MergedOverrides(std::vector<RE::NiAVObject*>& a_withWorld, std::vector<RE::NiAVObject*>& a_withRoom)
	{
		for (auto& walk : walks) {
			auto  actor = walk.actor.get();
			auto* object = actor ? actor->Get3D() : nullptr;
			if (object && !walk.crossed) {
				(walk.wentIn ? a_withWorld : a_withRoom).push_back(object);
			}
		}
	}

	json Status()
	{
		return { { "tracked", lastFrame.size() }, { "carried", carried }, { "last", lastCarried } };
	}
}
