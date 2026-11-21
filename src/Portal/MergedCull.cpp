#include "Portal/MergedCull.h"

#include "Portal/NpcTravel.h"
#include "Portal/PortalManager.h"
#include "Render/DoorwayPlanes.h"

namespace Portal::MergedCull
{
	namespace
	{
		using Render::Doorway::Plane;
		using Render::Doorway::Side;
		using Render::Doorway::SideOf;

		Mode                mode = Mode::kShapes;
		Mode                streetMode = Mode::kOff;
		std::vector<Room>   rooms;  // main thread, as is the view

		// Shown again after the view; only what was shown before, so the cull never undoes another
		// part's hiding (blockers in the doorway, shapes with stray bones).
		std::vector<RE::NiPointer<RE::NiAVObject>> hidden;
		bool                                       viewOpen = false;

		// The objects to leave alone, and every node above one (those can't be hidden whole).
		std::unordered_set<const RE::NiAVObject*> keep;
		std::unordered_set<const RE::NiAVObject*> aboveKept;

		Plane planes[5];

		// A bound this big or this far from the door is no bound of the room as merged (one the
		// engine fixed in the cell's own coordinates and not yet moved, see RemoteCell::MoveBounds):
		// it says nothing, so the node is looked into and a shape left drawn.
		constexpr float kImplausible = 20000.0f;
		RE::NiPoint3    doorCenter;

		// Skinned shapes' bounds follow their bones only loosely; a person is tested as a whole,
		// generously, and never shape by shape.
		constexpr float kPersonRadius = 160.0f;
		constexpr float kPersonCentreHeight = 70.0f;

		struct Stats
		{
			std::uint64_t views = 0;
			std::uint64_t cameraInside = 0;
			std::uint64_t noneSeen = 0;  // everything hidden
			std::uint64_t shapeCulls = 0;
			std::uint32_t lastTested = 0;
			std::uint32_t lastHidden = 0;
			std::string   lastWhy;
			// Last 120 views: CPU time of the whole player view (cull included) and of the cull.
			std::array<float, 120> viewMs{};
			std::array<float, 120> cullMs{};
			std::size_t            index = 0;
		} stats;
		std::chrono::steady_clock::time_point viewStarted;
		float                                 lastCullMs = 0.0f;

		// The rooms of the merged cells (from their portal graphs; a few dozen at most), for
		// RoomCullHook. Culls run on job threads too.
		std::mutex                         roomsLock;
		std::vector<const RE::NiAVObject*> mergedRooms;  // under roomsLock; compared, never followed

		bool IsMergedRoom(const RE::NiAVObject* a_node)
		{
			std::scoped_lock lock(roomsLock);
			return std::ranges::find(mergedRooms, a_node) != mergedRooms.end();
		}

		// An interior built of room bounds (Mistveil Keep) is culled through its portal graph, which
		// the world's scene doesn't have. Merged, the main view (ID 36643) missed two parts of it:
		// - A room's cull (ID 75886) walks into its children only when the culling process's graph
		//   lists the room, or when the process ignores multibounds. Nothing of the room was drawn
		//   but its particles (those come from the light scene, not the cull): the land behind the
		//   house showed through the doorway.
		// - What straddles a portal between two rooms (the great hall's walls) hangs under the
		//   portal's BSPortalSharedNode, in the first child of the cell's sixth node, which the main
		//   view leaves out: the graph culls it through the portals of the rooms seen.
		// For a merged room, kNormal processes ignore multibounds within the room, as the portal
		// pass does throughout (RenderPortal), and the room culls the shared geometry of its
		// portals, each portal from one of its two rooms only (so it is drawn once). Rooms are
		// always drawn (kAlwaysDraw), so every room of the cell gets here in every cull.
		struct RoomCullHook
		{
			static void Thunk(RE::BSMultiBoundRoom* a_room, RE::NiCullingProcess& a_process, std::int32_t a_alphaGroupIndex)
			{
				using Type = RE::BSCullingProcess::BSCPCullingType;
				// The engine's own function reads it as a BSCullingProcess, whichever process it is.
				auto& process = static_cast<RE::BSCullingProcess&>(a_process);
				if (process.cullMode.get() != Type::kNormal || !IsMergedRoom(a_room)) {
					func(a_room, a_process, a_alphaGroupIndex);
					return;
				}
				process.cullMode = Type::kIgnoreMultiBounds;
				func(a_room, a_process, a_alphaGroupIndex);
				for (auto* portal : a_room->portalList) {
					auto* shared = portal ? portal->portalSharedNode.get() : nullptr;
					if (!shared) {
						continue;
					}
					const auto* owner = portal->entranceRoom && IsMergedRoom(portal->entranceRoom) ? portal->entranceRoom : portal->exitRoom;
					if (owner == a_room) {
						static REL::Relocation<void (*)(RE::NiAVObject*, RE::NiCullingProcess&, std::uint32_t)> cull{ REL::ID(70267) };
						cull(shared, a_process, static_cast<std::uint32_t>(a_alphaGroupIndex));
					}
				}
				process.cullMode = Type::kNormal;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		void Hide(RE::NiAVObject* a_object)
		{
			if (!a_object->GetAppCulled()) {
				a_object->SetAppCulled(true);
				hidden.emplace_back(a_object);
			}
		}

		void HideAll(RE::NiAVObject* a_object)
		{
			++stats.lastTested;
			if (keep.contains(a_object)) {
				return;
			}
			if (!aboveKept.contains(a_object)) {
				Hide(a_object);
				return;
			}
			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					if (child) {
						HideAll(child.get());
					}
				}
			}
		}

		// Wholly outside X (hide), wholly inside (draw as it is), or neither.
		Side Classify(const RE::NiPoint3& a_center, float a_radius)
		{
			if (a_radius <= 0.0f || a_radius > kImplausible || a_center.GetDistance(doorCenter) > kImplausible) {
				return Side::kCrosses;
			}
			bool within = true;
			for (const auto& plane : planes) {
				const auto side = SideOf(plane, a_center, a_radius);
				if (side == Side::kOutside) {
					return Side::kOutside;
				}
				within &= side == Side::kInside;
			}
			return within ? Side::kInside : Side::kCrosses;
		}

		void Walk(RE::NiAVObject* a_object)
		{
			++stats.lastTested;
			if (keep.contains(a_object)) {
				return;
			}
			const bool guarded = aboveKept.contains(a_object);
			auto*      owner = a_object->GetUserData();
			if (auto* actor = owner ? owner->As<RE::Actor>() : nullptr; actor && actor->Get3D() == a_object) {
				const auto at = actor->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, kPersonCentreHeight };
				if (!guarded && Classify(at, std::max(kPersonRadius, a_object->worldBound.radius)) == Side::kOutside &&
					Classify(a_object->worldBound.center, a_object->worldBound.radius) == Side::kOutside) {
					Hide(a_object);
				}
				return;
			}
			const auto side = Classify(a_object->worldBound.center, a_object->worldBound.radius);
			if (side == Side::kInside) {
				return;
			}
			// Not a room whole: it culls its portals' shared geometry (RoomCullHook), which may be
			// seen when the room's bound isn't. Its contents are looked at one by one instead.
			if (side == Side::kOutside && !guarded && !netimmerse_cast<RE::BSMultiBoundRoom*>(a_object)) {
				Hide(a_object);
				return;
			}
			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					if (child) {
						Walk(child.get());
					}
				}
			}
		}

		// Some part of the doorway's rectangle may be in the camera's view: false only when all four
		// corners lie beyond one side of the view frustum. In camera space, not on screen: a corner
		// behind the camera has no meaningful screen position, and looking away from the door all
		// four are behind it.
		bool DoorwayInView(const Render::Doorway::Door& a_door)
		{
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return true;
			}
			const auto& frustum = camera->GetRuntimeData2().viewFrustum;
			if (frustum.bOrtho) {
				return true;
			}
			// A camera looks along its local X, with Y up and Z to the right.
			const auto&        m = camera->world.rotate;
			const RE::NiPoint3 forward{ m.entry[0][0], m.entry[1][0], m.entry[2][0] };
			const RE::NiPoint3 upward{ m.entry[0][1], m.entry[1][1], m.entry[2][1] };
			const RE::NiPoint3 rightward{ m.entry[0][2], m.entry[1][2], m.entry[2][2] };
			// Widened, so a doorway at the edge of the screen never pops.
			constexpr float kWiden = 1.15f;
			constexpr float kSlack = 30.0f;
			const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
			unsigned           outside = ~0u;  // bit per frustum side that every corner is beyond
			for (const float sx : { -1.0f, 1.0f }) {
				for (const float sy : { -1.0f, 1.0f }) {
					const auto  offset = a_door.center + a_door.right * (sx * a_door.halfWidth) + up * (sy * a_door.halfHeight) - camera->world.translate;
					const float f = offset.Dot(forward);
					const float u = offset.Dot(upward);
					const float r = offset.Dot(rightward);
					unsigned    beyond = 0;
					beyond |= f < -kSlack ? 1u : 0u;
					beyond |= r < frustum.fLeft * kWiden * f - kSlack ? 2u : 0u;
					beyond |= r > frustum.fRight * kWiden * f + kSlack ? 4u : 0u;
					beyond |= u < frustum.fBottom * kWiden * f - kSlack ? 8u : 0u;
					beyond |= u > frustum.fTop * kWiden * f + kSlack ? 16u : 0u;
					outside &= beyond;
				}
			}
			return outside == 0;
		}

		// The rooms are culled one after the other, each against its own doorway; they are disjoint
		// subtrees, so what one keeps says nothing about another.
		// a_unseen: no line of sight reaches the room (the camera stands in another one).
		void CullRoom(const Room& a_room, bool a_unseen)
		{
			const auto* room = &a_room;
			const auto door = Render::Doorway::Door::From(room->door);
			if (door.cameraMerged && mode != Mode::kHideAll) {
				// The camera stands in the room: it is the space seen everywhere.
				++stats.cameraInside;
				stats.lastWhy = "camera in the room";
				return;
			}
			// The street casts the sun's shadows (Interior Sun), into the room too: hidden whole while
			// the doorway was off screen, the sunlit patches on the room's walls changed; shape by
			// shape, a porch beam's shadow seen through the doorway thinned. Off unless asked for.
			const auto wanted = room->street && mode != Mode::kHideAll ? std::min(mode, streetMode) : mode;
			if (wanted == Mode::kOff) {
				stats.lastWhy = "street: not culled";
				return;
			}
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return;
			}
			doorCenter = door.center;
			const char* none = mode == Mode::kHideAll                               ? "hidden by hand" :
			                   a_unseen                                             ? "camera in another room" :
			                   room->doorShut                                      ? "door shut" :
			                   !Render::Doorway::Planes(door, camera->world.translate, planes) ? "camera behind the door plane" :
			                   !DoorwayInView(door)                                 ? "doorway off screen" :
			                                                                          nullptr;
			if (!none && wanted == Mode::kWhole) {
				stats.lastWhy = "doorway seen";
				return;
			}
			keep.clear();
			aboveKept.clear();
			std::vector<RE::NiAVObject*> withWorld, withRoom;
			Portal::NpcTravel::MergedOverrides(withWorld, withRoom);
			const auto keepObject = [](const RE::NiAVObject* a_object) {
				keep.insert(a_object);
				for (auto* up = a_object->parent; up; up = up->parent) {
					aboveKept.insert(up);
				}
			};
			for (const auto& object : room->keep) {
				keepObject(object.get());
			}
			// People walking in from the street, drawn with it until they pass the door plane.
			for (auto* object : withWorld) {
				keepObject(object);
			}
			auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
			const auto merged = Portal::Manager::MergedCells();
			for (auto* cell : room->cells) {
				// Only cells still merged: a room the player has just walked into is theirs now.
				if (cell == here || std::ranges::find(merged, cell) == merged.end()) {
					continue;
				}
				auto* loaded = cell->GetRuntimeData().loadedData;
				if (auto* root = loaded ? loaded->cell3D.get() : nullptr) {
					if (none) {
						HideAll(root);
					} else {
						Walk(root);
					}
				}
			}
			for (const auto& root : room->roots) {
				if (none) {
					HideAll(root.get());
				} else {
					Walk(root.get());
				}
			}
			stats.lastWhy = none ? none : "doorway seen";
			++(none ? stats.noneSeen : stats.shapeCulls);
		}

		void Cull()
		{
			stats.lastTested = 0;
			if (rooms.empty() || mode == Mode::kOff) {
				stats.lastWhy = rooms.empty() ? "nothing merged" : "off";
				return;
			}
			++stats.views;
			std::string why;
			const bool  cameraInRoom = std::ranges::any_of(rooms, [](const Room& a_room) { return Render::Doorway::Door::From(a_room.door).cameraMerged; });
			for (const auto& room : rooms) {
				CullRoom(room, cameraInRoom && !Render::Doorway::Door::From(room.door).cameraMerged);
				why += (why.empty() ? "" : "; ") + stats.lastWhy;
			}
			stats.lastWhy = std::move(why);
		}
	}

	void Install()
	{
		// AE IDs: the plugin runs on AE only.
		REL::Relocation<std::uintptr_t> vtable{ REL::ID(238170) };
		const auto                      expected = REL::Relocation<std::uintptr_t>{ REL::ID(75886) }.address();
		const auto                      slot = *reinterpret_cast<const std::uintptr_t*>(vtable.address() + 0x34 * sizeof(std::uintptr_t));
		if (slot != expected) {
			logger::warn("portal: BSMultiBoundRoom's OnVisible is not ID 75886; merged rooms built of room bounds stay unseen");
			return;
		}
		RoomCullHook::func = vtable.write_vfunc(0x34, RoomCullHook::Thunk);
	}

	void Set(std::vector<Room> a_rooms)
	{
		rooms = std::move(a_rooms);
		// Not the room the player has walked into: the engine culls it with its own graph then.
		auto*                              here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		std::vector<const RE::NiAVObject*> found;
		for (const auto& room : rooms) {
			for (auto* cell : room.cells) {
				auto* loaded = cell != here ? cell->GetRuntimeData().loadedData : nullptr;
				if (auto* graph = loaded ? loaded->portalGraph.get() : nullptr) {
					for (const auto& node : graph->rooms) {
						found.push_back(node.get());
					}
				}
			}
		}
		std::scoped_lock lock(roomsLock);
		mergedRooms = std::move(found);
	}

	void BeforeView()
	{
		viewStarted = std::chrono::steady_clock::now();
		if (viewOpen) {
			AfterView();  // not closed last time; the hidden must not stay hidden
		}
		viewOpen = true;
		try {
			Cull();
		} catch (...) {
			// A view without the cull is only slower.
		}
		stats.lastHidden = static_cast<std::uint32_t>(hidden.size());
		lastCullMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - viewStarted).count();
	}

	void AfterView()
	{
		for (auto& object : hidden) {
			object->SetAppCulled(false);
		}
		hidden.clear();
		if (std::exchange(viewOpen, false)) {
			const auto slot = stats.index++ % stats.viewMs.size();
			stats.viewMs[slot] = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - viewStarted).count();
			stats.cullMs[slot] = lastCullMs;
		}
	}

	void SetMode(Mode a_mode)
	{
		mode = a_mode;
		// The view times are compared per mode.
		stats.index = 0;
	}

	Mode GetMode()
	{
		return mode;
	}

	void SetStreetMode(Mode a_mode)
	{
		streetMode = a_mode;
		stats.index = 0;
	}

	json Describe()
	{
		const auto count = std::min(stats.index, stats.viewMs.size());
		const auto summary = [&](const std::array<float, 120>& a_ms) -> json {
			if (!count) {
				return nullptr;
			}
			std::vector<float> sorted(a_ms.begin(), a_ms.begin() + count);
			std::ranges::sort(sorted);
			return { { "avg", std::accumulate(sorted.begin(), sorted.end(), 0.0f) / count }, { "median", sorted[count / 2] }, { "p95", sorted[count * 95 / 100] },
				{ "max", sorted.back() } };
		};
		return {
			{ "mode", static_cast<int>(mode) },
			{ "streetMode", static_cast<int>(streetMode) },
			{ "merged", rooms.size() },
			{ "views", stats.views },
			{ "cameraInside", stats.cameraInside },
			{ "noneSeen", stats.noneSeen },
			{ "shapeCulls", stats.shapeCulls },
			{ "lastWhy", stats.lastWhy },
			{ "lastTested", stats.lastTested },
			{ "lastHidden", stats.lastHidden },
			{ "viewMs", summary(stats.viewMs) },
			{ "cullMs", summary(stats.cullMs) },
		};
	}
}
