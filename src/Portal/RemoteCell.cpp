#include "Portal/RemoteCell.h"

#include "Hooks.h"
#include "Portal/DoorPair.h"
#include "Portal/LightScene.h"
#include "Probe.h"

namespace
{
	using CellState = RE::TESObjectCELL::CellState;

	std::vector<RE::Actor*> ActorsIn(const RE::TESObjectCELL* a_cell)
	{
		std::vector<RE::Actor*> actors;
		a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			if (actor && !actor->IsPlayerRef()) {
				actors.push_back(actor);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return actors;
	}

	// The land's own fade-in: BSLightingShaderProperty + 0x108 of each land quad, which the depth
	// prepass takes as the strength of its dithered alpha mask (ID 107762 + 0xB84, AlphaTestRef.w,
	// for multi-texture landscape). The engine raises it from 0 only for the cells of its own grid.
	// At 0 the prepass dithered the whole quad away and the lighting pass (depth test EQUAL) drew
	// nothing there: through the doorway the distant land showed where the street's ground was.
	void ShowLand(RE::TESObjectCELL* a_cell)
	{
		auto* land = a_cell && a_cell->IsExteriorCell() ? a_cell->GetRuntimeData().cellLand : nullptr;
		auto* data = land ? land->loadedData : nullptr;
		for (int quad = 0; data && quad < 4; ++quad) {
			if (!data->mesh[quad]) {
				continue;
			}
			RE::BSVisit::TraverseScenegraphGeometries(data->mesh[quad], [](RE::BSGeometry* a_shape) {
				if (auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(a_shape->GetGeometryRuntimeData().shaderProperty.get())) {
					property->unk108 = 1.0f;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}
}

namespace Portal
{
	RE::NiAVObject* RemoteCell::Cell3D(const RE::TESObjectCELL* a_cell)
	{
		auto* loaded = a_cell ? a_cell->GetRuntimeData().loadedData : nullptr;
		return loaded ? loaded->cell3D.get() : nullptr;
	}

	RE::NiAVObject* RemoteCell::Cell3D() const
	{
		return Cell3D(Cell());
	}

	bool RemoteCell::Attachable(const RE::TESObjectCELL* a_cell)
	{
		// Attached/Attaching by the engine means it's (becoming) a current cell; never steal it.
		const auto state = a_cell ? a_cell->cellState.get() : CellState::kAttached;
		return a_cell && a_cell != RE::PlayerCharacter::GetSingleton()->GetParentCell() && state != CellState::kAttached && state != CellState::kAttaching &&
		       state != CellState::kDetaching;
	}

	RemoteCell::PersistentByCell RemoteCell::PersistentIn(const std::vector<RE::TESObjectCELL*>& a_cells)
	{
		PersistentByCell found;
		auto*            first = a_cells.empty() ? nullptr : a_cells.front();
		auto*            world = first && !first->IsInteriorCell() ? first->GetRuntimeData().worldSpace : nullptr;
		if (!world || !world->persistentCell) {
			return found;
		}
		const auto key = [](std::int32_t a_x, std::int32_t a_y) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x)) << 32) | static_cast<std::uint32_t>(a_y); };
		std::unordered_map<std::uint64_t, const RE::TESObjectCELL*> byCoordinates;
		for (auto* cell : a_cells) {
			if (auto* coordinates = cell->IsInteriorCell() ? nullptr : cell->GetCoordinates()) {
				byCoordinates.emplace(key(coordinates->cellX, coordinates->cellY), cell);
			}
		}
		world->persistentCell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			if (!a_ref || !a_ref->Get3D()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto pos = a_ref->GetPosition();
			const auto cell = byCoordinates.find(key(static_cast<std::int32_t>(std::floor(pos.x / 4096.0f)), static_cast<std::int32_t>(std::floor(pos.y / 4096.0f))));
			if (cell != byCoordinates.end()) {
				found[cell->second].push_back(a_ref->GetFormID());
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return found;
	}

	int RemoteCell::AdoptPersistent(RE::TESObjectCELL* a_cell, const PersistentByCell& a_persistent)
	{
		// Outdoors, persistent references (a house, its porch: Riverwood's Trader) belong to the
		// worldspace's persistent cell; the grid attaches their 3D to the cell they stand in. When the
		// player goes inside, the engine takes them off it (some frames after the cell change), and
		// the cell alone left a hole where the building was. Their 3D still exists, unparented:
		// hang it under the cell again. Repeated from Update for the ones taken off later.
		auto* cell3D = Cell3D(a_cell) ? Cell3D(a_cell)->AsNode() : nullptr;
		const auto refs = a_persistent.find(a_cell);
		if (!cell3D || refs == a_persistent.end()) {
			return 0;
		}
		auto* root = scene.get();
		auto* havokWorld = a_cell->GetbhkWorld();
		int   restored = 0;
		int   hung = 0;
		for (const auto id : refs->second) {
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
			auto* object = ref ? ref->Get3D() : nullptr;
			if (!object) {
				continue;
			}
			// A town's people are persistent: not in the cell's own list, so not raised to the high
			// process with the cell's actors (AttachOne). Left in the low process the engine gives
			// them while the player is indoors, they moved in a jump every few seconds and didn't
			// animate, seen through the doorway.
			if (auto* actor = ref->As<RE::Actor>(); actor && stepExterior && !actor->IsDead() && !(debugSkip & 256)) {
				auto* process = actor->GetActorRuntimeData().currentProcess;
				if (!process || !process->InHighProcess()) {
					actor->MoveToHigh();
				}
			}
			// The 3D may hang in a node the engine took off the scene as a whole (the persistent
			// cell's own); that node is what goes under the portal root.
			RE::NiAVObject* top = object;
			while (top->parent) {
				top = top->parent;
			}
			if (top != root && top->AsNode() != RE::Main::WorldRootNode()) {
				root->AttachChild(top, true);
				borrowedPersistent.emplace_back(top);
				++hung;
			}
			// Their collision went with them: the engine took their bodies out of the worldspace's
			// Havok world too. Whiterun's market street is one (WRMainRoadMarket): simulated from
			// indoors, the people walking on it sank through it to the land 100 units below, and
			// were seen up to their heads in the ground, also after walking out. Put back in.
			// Not a door held open passable, whose bodies were taken out on purpose (a city gate's).
			if (havokWorld && !ref->As<RE::Actor>() && !Portal::Doors::BodiesHidden(ref->GetFormID())) {
				RE::BSVisit::TraverseScenegraphObjects(object, [&](RE::NiAVObject* a_object) {
					auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
					auto* body = collision ? collision->body.get() : nullptr;
					// (Nor one kept out of the gate's passage, ClearPassage.)
					if (body && !body->GetWorld1() && std::ranges::none_of(passageCleared, [&](const auto& a_cleared) { return a_cleared.get() == body; })) {
						body->MoveToWorld(havokWorld);
						++restored;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}
		if (restored) {
			logger::info("portal: {} collision object(s) of persistent references put back into {:08X}'s Havok world", restored, a_cell->GetFormID());
		}
		return hung;
	}

	void RemoteCell::AttachOne(RE::TESObjectCELL* a_cell)
	{
		// 19111 loads the cell's temporary references (no-op when loaded); 18976 attaches it
		// under a parent node of our choosing; 19063 finishes the attach (state -> Attached).
		REL::Relocation<bool (*)(RE::TESObjectCELL*)>               loadTempData{ REL::ID(19111) };
		REL::Relocation<void (*)(RE::TESObjectCELL*, RE::NiNode*)> attachCell{ REL::ID(18976) };
		REL::Relocation<std::uint8_t (*)(RE::TESObjectCELL*, bool)> finishAttach{ REL::ID(19063) };
		// The attach installs the cell's room/portal occlusion graph in the *world* light scene
		// (ShadowSceneNode::SetPortalGraph on State slot 0). Left there, the main view culled with
		// the remote cell's rooms: actors outside them (the player, every NPC) vanished, and the
		// main view accumulated the remote cell's geometry too, colliding with the portal pass's
		// render passes and crashing. Put the world's graph back. Deliberately no graph on the
		// portal scene: a light scene with a graph expects the culling process to carry a
		// portal-graph entry (ID 106322), which our cull context doesn't, and it crashed; without
		// one the remote cell's rooms render fine.
		auto* worldScene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto* worldGraph = worldScene ? worldScene->GetRuntimeData().portalGraph : nullptr;
		loadTempData(a_cell);
		attachCell(a_cell, scene.get());
		finishAttach(a_cell, true);
		// The land's meshes are built only when the engine loads a cell into its own grid (IDs 13285
		// and 13427 call this right after 19063), which a remote cell never goes through: a street
		// read from disk while the player is indoors (a game loaded in a room) had no ground through
		// the doorway until the player had been outside once. A land already built is left as it is.
		// Not GetLand(): it makes an empty land for a cell whose own is not loaded, built flat at
		// the engine's "no data" height of -27000 units.
		auto* land = a_cell->IsExteriorCell() ? a_cell->GetRuntimeData().cellLand : nullptr;
		if (land && land->data.flags.any(RE::OBJ_LAND::Flag::kVertexNormals_HeightMap, RE::OBJ_LAND::Flag::kVertexColors, RE::OBJ_LAND::Flag::kLayers, RE::OBJ_LAND::Flag::kMPCD)) {
			land->LoadVertices(true, true);
		}
		ShowLand(a_cell);
		if (worldScene) {
			worldScene->SetPortalGraph(worldGraph);
		}
		cells.push_back(a_cell);
		held.push_back(a_cell);

		// High process so the engine runs full AI, animation and movement for its NPCs; it does
		// not demote them again while the cell stays attached.
		for (auto* actor : ActorsIn(a_cell)) {
			if (!actor->IsDead() && !(debugSkip & 256)) {
				actor->MoveToHigh();
			}
		}
		// A light ref creates its light when its 3D loads. In a cell that is still loaded from an
		// earlier visit nothing loads, and the lights the engine removed when the player left were
		// never made again: the portal showed it unlit. TES::SetCurrentCell recreates them with
		// ID 19059 (cell, true), which runs ID 19678 (CreatePointLight) for each reference with 3D;
		// they land in the world scene and are adopted like any other.
		REL::Relocation<void (*)(RE::TESObjectCELL*, bool)> attachRefLights{ REL::ID(19059) };
		if (!(debugSkip & 2)) {
			attachRefLights(a_cell, true);
		}
		if (!(debugSkip & 4)) {
			adoptedLights += LightScene::Adopt(scene.get(), Cell3D(a_cell));
		}
	}

	bool RemoteCell::Attach(RE::TESObjectCELL* a_cell, bool a_toMerge, bool a_show)
	{
		if (!cells.empty() || !Attachable(a_cell)) {
			return false;
		}
		scene = LightScene::Create();
		if (a_show) {
			LightScene::Show(scene.get());
		}
		generation = ++lastGeneration;
		adoptedLights = 0;
		persistentCheck = 0.0f;
		roundNext = 0;
		roundPersistent.clear();
		attachedAt = std::chrono::steady_clock::now();
		handedOver.clear();
		// A cell still loaded from the player's visit (they just came through the door) has nothing
		// left to build; the long wait is for cells whose 3D is being loaded now.
		bool preloaded = Cell3D(a_cell) != nullptr;
		// Attaching a cell, and fetching an exterior's neighbours, empties the world scene of its
		// lights (25 -> 0 in a lit room: seen from inside, every candle and fire went dark for
		// good, since nothing makes them again while the cell stays loaded). They are put back.
		const auto worldLights = LightScene::WorldLights();
		// The door's cell can be in memory without having been read from the plugin files: a game
		// loaded indoors holds the exterior cells of persistent references (the door) as shells, with
		// no land and no temporary references. Read like its neighbours below (a no-op when it was).
		if (!a_cell->IsInteriorCell() && !(debugSkip & 8)) {
			auto* world = a_cell->GetRuntimeData().worldSpace;
			auto* coordinates = a_cell->GetCoordinates();
			if (world && coordinates && world->LoadCell(static_cast<std::int16_t>(coordinates->cellX), static_cast<std::int16_t>(coordinates->cellY)) != a_cell) {
				logger::warn("portal: {} is not the worldspace's cell at its coordinates", Probe::DescribeCell(a_cell));
			}
		}
		AttachOne(a_cell);
		// Outdoors the doorway looks out over more than the one cell its door is in: an exterior
		// cell's land and objects only reach to its border, so the neighbours are attached too.
		if (!a_cell->IsInteriorCell() && !(debugSkip & 8)) {
			auto* world = a_cell->GetRuntimeData().worldSpace;
			auto* coordinates = a_cell->GetCoordinates();
			if (world && coordinates) {
				for (int dy = -kNeighbourRadius; dy <= kNeighbourRadius; ++dy) {
					for (int dx = -kNeighbourRadius; dx <= kNeighbourRadius; ++dx) {
						if (!dx && !dy) {
							continue;
						}
						// Loaded from disk if needed (a game loaded indoors); the 3D then loads like
						// the door's own cell's, and the portal waits for all of them.
						auto* neighbour = world->LoadCell(static_cast<std::int16_t>(coordinates->cellX + dx), static_cast<std::int16_t>(coordinates->cellY + dy));
						if (neighbour && Attachable(neighbour)) {
							preloaded &= Cell3D(neighbour) != nullptr;
							AttachOne(neighbour);
						}
					}
				}
			}
		}
		// All the cells' persistent references in one pass over the worldspace's persistent cell
		// (thousands of references in a city): done per cell, it was most of an attach.
		if (!(debugSkip & 1)) {
			const auto persistent = PersistentIn(cells);
			for (auto* cell : cells) {
				// Their lights (a lantern on a house) come with them.
				if (AdoptPersistent(cell, persistent) && !(debugSkip & 4)) {
					adoptedLights += LightScene::Adopt(scene.get(), Cell3D(cell));
				}
			}
		}
		if (auto* worldScene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			const auto kept = LightScene::WorldLights();
			// A light of the attached cells left the world scene because it was adopted into the
			// portal's (a cell the player left a moment ago can still have its lights there); put
			// back, it would be registered in both.
			const auto ofRemote = [&](const RE::NiPointer<RE::BSLight>& a_light) {
				for (const RE::NiAVObject* node = a_light->light.get(); node; node = node->parent) {
					if (std::ranges::any_of(cells, [&](RE::TESObjectCELL* a_cell) { return Cell3D(a_cell) == node; })) {
						return true;
					}
				}
				return false;
			};
			int restored = 0;
			for (const auto& light : worldLights) {
				if (std::ranges::find(kept, light) == kept.end() && !ofRemote(light)) {
					worldScene->AddLight(light.get());
					++restored;
				}
			}
			if (restored) {
				logger::info("portal: {} world light(s) put back after the attach", restored);
			}
		}
		// An interior is merged and must be there in the frame of arrival (the camera is standing in
		// it). An exterior goes through the portal pass, which has drawn stale render passes of
		// cells attached in the very frame the player left them: it waits a moment. Not so a street
		// merged into the interior just walked into: until it was, the room was lit without the
		// street's houses around it (Warmaiden's with the daylight of the lights outside its door
		// on the floor), and changed half a second after walking in.
		quietRequired = !preloaded ? kQuietSecondsRequired : a_cell->IsInteriorCell() || a_toMerge ? kQuietSecondsPreloaded : kQuietSecondsLeftCell;
		steppedFrames = 0;
		quietSeconds = 0.0f;
		// Per remote space: carried over, a queue the previous cell left stuck at the same count
		// made the next one count as loaded before it had loaded anything.
		lastQueued = 0;
		stuckSeconds = 0.0f;
		ready = false;
		readySeconds = 0.0f;
		itemsChecked = false;
		groundFound = false;
		groundCheck = 0.0f;
		logger::info("portal: attached {} (+{} neighbouring cells, {} lights)", Probe::DescribeCell(a_cell), cells.size() - 1, adoptedLights);
		return true;
	}

	// Moving a whole cell (merging it, or putting it back) changes every object's world transform
	// in one step, and static objects are never updated again: their "previous" transform, which
	// the shaders turn into motion vectors, stays where the cell came from, tens of thousands of
	// units away. The upscaler then has no history for the room (grainy), and frame generation
	// draws warped, layered copies of it. The previous transform is set to the current one.
	void SettleMotion(RE::NiAVObject* a_root)
	{
		int stale = 0;
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			stale += a_object->previousWorld.translate.GetDistance(a_object->world.translate) > 10.0f ? 1 : 0;
			a_object->previousWorld = a_object->world;
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		logger::info("portal: {} object(s) had a previous transform from before the move", stale);
	}

	bool RemoteCell::Healthy() const
	{
		if (merged) {
			return !cells.empty();
		}
		auto* root = scene.get();
		return root && !cells.empty() && std::ranges::all_of(cells, [&](RE::TESObjectCELL* a_cell) {
			auto* cell3D = Cell3D(a_cell);
			return a_cell->cellState.get() == CellState::kAttached && cell3D && cell3D->parent == root;
		});
	}

	bool RemoteCell::FullyLoaded() const
	{
		return !cells.empty() && ready && (slowLoadMs <= 0 || std::chrono::steady_clock::now() - attachedAt >= std::chrono::milliseconds(slowLoadMs));
	}

	void RemoteCell::ClearPassage(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_halfHeight)
	{
		if (!passageCleared.empty() || cells.empty()) {
			return;
		}
		passageWorld = cells.front()->GetbhkWorld();
		if (!passageWorld) {
			return;
		}
		// The passage: from well in front of the doorway (the player's side) to a little past it,
		// as wide and high as the doorway with some room.
		constexpr float kFront = 200.0f, kBehind = 30.0f, kMargin = 60.0f;
		const RE::NiPoint3 side{ a_inward.y, -a_inward.x, 0.0f };
		const auto         touches = [&](const RE::NiBound& a_bound) {
			const auto  offset = a_bound.center - a_center;
			const float depth = offset.x * a_inward.x + offset.y * a_inward.y;
			const float across = std::abs(offset.x * side.x + offset.y * side.y);
			const float up = std::abs(offset.z);
			const float r = a_bound.radius;
			return depth + r > -kFront && depth - r < kBehind && across - r < a_halfWidth + kMargin && up - r < a_halfHeight + kMargin;
		};
		const auto clear = [&](RE::NiAVObject* a_root) {
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
				auto* collision = a_object->collisionObject ? a_object->collisionObject->AsBhkNiCollisionObject() : nullptr;
				auto* body = collision ? collision->body.get() : nullptr;
				if (body && body->GetWorld1() && touches(a_object->worldBound)) {
					passageCleared.emplace_back(body);
					body->RemoveFromCurrentWorld();
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		};
		for (auto* cell : cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* object = a_ref ? a_ref->Get3D() : nullptr;
				// Not the people (they stand on it), nor doors (Doors::Open), nor the land.
				if (object && !a_ref->As<RE::Actor>() && !(a_ref->GetBaseObject() && a_ref->GetBaseObject()->Is(RE::FormType::Door)) && touches(object->worldBound)) {
					clear(object);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		for (auto& top : borrowedPersistent) {
			if (top && touches(top->worldBound)) {
				clear(top.get());
			}
		}
		logger::info("portal: {} body(ies) of {} kept out of the gate's passage", passageCleared.size(), Probe::DescribeCell(cells.front()));
	}

	void RemoteCell::RestorePassage()
	{
		for (auto& body : passageCleared) {
			if (passageWorld && !body->GetWorld1()) {
				body->MoveToWorld(passageWorld);
			}
		}
		passageCleared.clear();
		passageWorld = nullptr;
	}

	void RemoteCell::SetParked(bool a_parked)
	{
		if (!a_parked) {
			for (auto& object : parked) {
				object->SetAppCulled(false);
			}
			parked.clear();
			return;
		}
		if (!merged || !parked.empty()) {
			return;
		}
		for (auto* cell : cells) {
			// Only what isn't hidden already, so that showing it again undoes nothing of the engine's.
			if (auto* cell3D = Cell3D(cell); cell3D && !cell3D->GetAppCulled()) {
				cell3D->SetAppCulled(true);
				parked.emplace_back(cell3D);
			}
		}
	}

	void RemoteCell::HangAlwaysRender(RE::TESObjectCELL* a_cell)
	{
		auto* loaded = a_cell->GetRuntimeData().loadedData;
		auto* graph = loaded ? loaded->portalGraph.get() : nullptr;
		auto* cell3D = Cell3D(a_cell) ? Cell3D(a_cell)->AsNode() : nullptr;
		if (!graph || !cell3D) {
			return;
		}
		auto holder = std::ranges::find_if(alwaysRenderHolders, [&](const auto& a_holder) { return a_holder->parent == cell3D; });
		int  hung = 0;
		// Also those still waiting to be taken in: the graph takes in 3D finished after the attach
		// only while it is the scene's (Mistveil Keep's steps, loaded a moment late).
		std::vector<RE::NiAVObject*> objects;
		for (const auto* list : { &graph->alwaysRenderChildren, &graph->pendingAlwaysRenderAdds }) {
			for (const auto& object : *list) {
				objects.push_back(object.get());
			}
		}
		for (const auto& removed : graph->pendingAlwaysRenderRemoves) {
			std::erase(objects, removed.get());
		}
		for (auto* object : objects) {
			if (!object || object->parent) {
				continue;
			}
			if (holder == alwaysRenderHolders.end()) {
				RE::NiPointer<RE::NiNode> node(RE::NiNode::Create());
				node->name = "SeamlessAlwaysRender";
				// At the end: the main view's cull goes by the cell node's slots and leaves some
				// out (ID 36643), a free slot among them.
				cell3D->AttachChild(node.get(), false);
				alwaysRenderHolders.push_back(node);
				holder = std::prev(alwaysRenderHolders.end());
			}
			(*holder)->AttachChild(object, true);
			++hung;
		}
		if (hung) {
			RE::NiUpdateData update;
			(*holder)->Update(update);
			logger::info("portal: {} always-drawn object(s) of {} hung into its 3D", hung, Probe::DescribeCell(a_cell));
		}
	}

	void RemoteCell::UnhangAlwaysRender()
	{
		// Back as the engine keeps them, unparented, where they stand in their cell.
		for (auto& holder : alwaysRenderHolders) {
			std::vector<RE::NiPointer<RE::NiAVObject>> children;
			for (auto& child : holder->GetChildren()) {
				if (child) {
					children.push_back(child);
				}
			}
			for (auto& child : children) {
				holder->DetachChild(child.get());
				RE::NiUpdateData update;
				child->Update(update);
			}
			if (holder->parent) {
				holder->parent->DetachChild(holder.get());
			}
		}
		alwaysRenderHolders.clear();
	}

	void RemoteCell::Release(bool a_handOver)
	{
		if (cells.empty()) {
			return;
		}
		// Hidden, the cell would stay so once the engine has it again (the player walking in).
		SetParked(false);
		// Before the cells go: handed over, they are the player's own again.
		RestorePassage();
		UnhangAlwaysRender();
		auto* tes = RE::TES::GetSingleton();
		auto* engineRoot = tes ? tes->objRoot : nullptr;
		// A merged street (merged into the interior being left) is put back under the portal root,
		// unmoved, below and then handed over like any other.
		const bool handOver = a_handOver && handOverStreet && engineRoot && !cells.front()->IsInteriorCell();
		// The step job may be running right now; it must be done with this world first.
		StepWorld(nullptr);
		auto* root = scene.get();
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		for (auto& [object, flags] : stilledClutter) {
			if (auto* collision = object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(object->collisionObject.get()) : nullptr) {
				*reinterpret_cast<std::uint32_t*>(&collision->flags) = flags;
			}
		}
		stilledClutter.clear();
		UnmergeCollision();
		if (std::exchange(merged, false)) {
			// Back under the portal root, unmoved, so the normal release below applies.
			for (auto* cell : cells) {
				RE::NiPointer<RE::NiAVObject> cell3D(Cell3D(cell));
				if (cell3D && cell3D->parent && cell3D->parent != root && cell != here) {
					cell3D->parent->DetachChild(cell3D.get());
					cell3D->local = RE::NiTransform();
					root->AttachChild(cell3D.get(), true);
					RE::NiUpdateData update;
					cell3D->Update(update);
					RecomputeBounds(cell3D.get());
					SettleMotion(cell3D.get());
				}
			}
		}
		RestoreBounds();
		// Our lights are copies of the cells' light refs; entering a cell normally re-creates them
		// in the world scene, so they must not stay registered here too.
		LightScene::Clear(scene.get());
		// Persistent references' 3D goes back to how the engine left it (unparented); the grid
		// attaches it again when the player comes back out. Only if it is still ours: if the
		// engine has hung it into the world again meanwhile (the player came back out another
		// way), taking it off its parent would leave a hole where the building is.
		for (auto& object : borrowedPersistent) {
			if (object->parent == root) {
				root->DetachChild(object.get());
			}
		}
		borrowedPersistent.clear();
		handedOver.clear();
		for (auto* cell : cells | std::views::reverse) {
			auto* cell3D = Cell3D(cell);
			if (handOver && cell3D && cell3D->parent == root && cell->cellState.get() == CellState::kAttached) {
				RE::NiPointer<RE::NiAVObject> keep(cell3D);
				root->DetachChild(cell3D);
				engineRoot->AttachChild(cell3D, true);
				handedOver.push_back(cell);
			} else if (cell3D && cell3D->parent == root && cell != here) {
				// Off our root *before* the engine detach: detaching with cell3D still under our root
				// crashed seconds later (S2f). If the engine has meanwhile re-parented the cell (the
				// player walked in normally), it owns it now and we only forget it.
				root->DetachChild(cell3D);
				REL::Relocation<void (*)(RE::TESObjectCELL*, bool)> detachCell{ REL::ID(19130) };
				detachCell(cell, true);
			} else {
				logger::info("portal: forgot {} (taken over by the engine)", Probe::DescribeCell(cell));
			}
		}
		for (auto* cell : cells) {
			std::erase(held, cell);
		}
		logger::info("portal: released {} (+{} neighbouring cells, {} handed over attached)", Probe::DescribeCell(cells.front()), cells.size() - 1, handedOver.size());
		cells.clear();
		roundNext = 0;
		roundPersistent.clear();
		fadeForced = false;
		adoptedLights = 0;
		LightScene::Retire(std::move(scene));
	}

	void RemoteCell::Update(float a_delta)
	{
		if (!Healthy()) {
			StepWorld(nullptr);
			return;
		}
		// Readiness. refsFullyLoaded is useless here: the engine sets it (ID 19146, which also fires
		// TESCellFullyLoadedEvent) only from its per-frame update of *current* cells (ID 18895). The
		// load queues alone aren't enough either: right after the attach they are still 0 because
		// nothing has been queued yet, and drawing then crashed on half-built geometry. So require
		// them to stay empty for a while.
		bool quiet = std::ranges::all_of(cells, [](RE::TESObjectCELL* a_cell) {
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			return loaded && loaded->queuedRefCount == 0 && loaded->criticalQueuedRefCount == 0;
		});
		// A reference can stay queued for good in a cell the player isn't in (seen with actors of
		// a random encounter standing at Whiterun's gate: no 3D, ever); the doorway then never
		// opened. Queues that haven't moved for a while count as done.
		int queued = 0;
		for (auto* cell : cells) {
			if (auto* loaded = cell->GetRuntimeData().loadedData) {
				queued += static_cast<int>(loaded->queuedRefCount) + static_cast<int>(loaded->criticalQueuedRefCount);
			} else {
				queued = -1;
				break;
			}
		}
		stuckSeconds = queued > 0 && queued == lastQueued ? stuckSeconds + a_delta : 0.0f;
		lastQueued = queued;
		quiet |= stuckSeconds > 5.0f;
		quietSeconds = quiet ? quietSeconds + a_delta : 0.0f;
		// Ready once, ready until released. References loading later (someone walking into the
		// street) are drawn once the engine has attached their finished 3D; counting them as "not
		// loaded" again blanked the whole doorway until the queues had been quiet for a while.
		if (!ready && quietSeconds >= quietRequired) {
			ready = true;
			logger::info("portal: remote cell ready");
		}
		if (ready && (readySeconds += a_delta) > 2.0f && !std::exchange(itemsChecked, true)) {
			CheckLooseItems();
		}
		int late = 0;
		// Every quarter of a second each cell is looked over (Check, below): one cell per frame,
		// a round at a time. All nine of a street in one frame took ~2 ms, four times a second,
		// felt as judder. While the space is still loading every cell is looked over every frame.
		const bool loading = quietSeconds < 3.0f;
		fadeForced = loading;
		// Nobody sees it: lights animated and a world stepped for no one were most of what a kept
		// room cost each frame. Still loading, it carries on, or it would be found half-built when
		// woken. Lights created meanwhile are taken in once it is woken.
		if (paused && !loading) {
			StepWorld(nullptr);
			return;
		}
		if (roundNext == 0 && (persistentCheck -= a_delta) <= 0.0f) {
			persistentCheck = 0.25f;
			roundNext = cells.size();
			roundPersistent = debugSkip & 1 ? PersistentByCell{} : PersistentIn(cells);
		}
		RE::TESObjectCELL* checked = nullptr;
		if (roundNext > 0) {
			checked = cells[cells.size() - roundNext--];
			AdoptPersistent(checked, roundPersistent);
		}
		for (auto* cell : cells) {
			// The engine fades references in only in the cells the player is in; in a remote cell
			// freshly loaded 3D would stay invisible (after loading a game indoors, most of the
			// village outside the door was missing).
			if (cell == checked || loading) {
				if (cell->IsInteriorCell() && cell != RE::PlayerCharacter::GetSingleton()->GetParentCell()) {
					HangAlwaysRender(cell);
				}
				if (auto* cell3D = Cell3D(cell)) {
					RE::BSVisit::TraverseScenegraphObjects(cell3D, [](RE::NiAVObject* a_object) {
						if (auto* fade = a_object->AsFadeNode()) {
							fade->GetRuntimeData().currentFade = 1.0f;
						}
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
				if (!cell->IsInteriorCell()) {
					StillLooseObjects(cell);
					ShowLand(cell);
				}
			}
			// A merged street's lights were adopted before the merge. In the world scene the engine
			// registers one of them anew every frame; taking each into ours, the world scene's queued
			// removal of the one before crashed (ID 106335 -> 106342).
			if (!(debugSkip & 4) && !(merged && !cell->IsInteriorCell())) {
				late += LightScene::Adopt(scene.get(), Cell3D(cell));
			}
			// A street merged into an interior hangs in the world scene: the light update there queued
			// a new light in the world's light scene every frame, which was adopted here, and the
			// world scene's queued removal of the one before crashed (ID 106335 -> 106342).
			if ((debugSkip & 16) || (merged && !cell->IsInteriorCell())) {
				continue;
			}
			// The engine only animates the lights of references in current cells; a light that
			// flickers or pulses (most of a Lux interior) starts with a negative fade as "not
			// updated yet" and stays dark until its reference's light update runs.
			cell->ForEachReference([](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (base && base->Is(RE::FormType::Light) && a_ref->Get3D()) {
					a_ref->UpdateRefLight();
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		if (late) {
			adoptedLights += late;
			logger::info("portal: adopted {} late light(s), {} in total", late, adoptedLights);
		}
		// The Havok step itself happens in the engine's step job (see StepHook) or at the end of
		// the frame; this only says which world to step. An exterior cell's world is the whole
		// worldspace's, which the engine leaves alone while the player is indoors (see stepExterior).
		auto* primary = cells.front();
		// The street seen from indoors is only simulated once there is ground under its NPCs and
		// loose items: stepped from the moment it was attached, they fell through it (people
		// vanishing, weapons and food missing from the street). Looked for twice a second until found.
		if (!primary->IsInteriorCell() && !groundFound && ready && (groundCheck -= a_delta) <= 0.0f) {
			groundCheck = 0.5f;
			groundFound = GroundUnderStreet();
			if (groundFound) {
				logger::info("portal: ground under the street; simulating it");
			}
		}
		// Diagnostics (16384): a space kept out of sight isn't simulated.
		const bool simulate = stepEnabled && (Shown() || !(debugSkip & 16384));
		StepWorld(simulate && (primary->IsInteriorCell() || (stepExterior && !stepInJob && groundFound)) ? primary->GetbhkWorld() : nullptr);
	}

	void RemoteCell::Show()
	{
		// Lights adopted while the scene wasn't registered wait in its add queue; registered, the
		// engine takes them in on its next light update. (Their light-geometry pairs stay empty,
		// unlike a space attached shown; merged rooms are lit by distance in the CS patch, and
		// looked the same.)
		LightScene::Show(scene.get());
	}

	void RemoteCell::StepWorld(RE::bhkWorld* a_world)
	{
		// Only on a change: the lock is held by the step job for as long as it steps, and the main
		// thread waiting on it every frame would wait for the physics.
		if (a_world == stepping || cells.empty()) {
			return;
		}
		std::scoped_lock lock(stepLock);
		std::erase_if(stepWorlds, [&](const auto& a_entry) { return a_entry.first == cells.front(); });
		if (a_world) {
			stepWorlds.emplace_back(cells.front(), a_world);
		}
		stepping = a_world;
	}

	namespace
	{
		using Step_t = void (*)(RE::bhkWorld*, std::uint32_t);

		void Step(RE::bhkWorld* a_world, std::uint32_t a_arg)
		{
			// bhkWorld vfunc 0x32 = per-frame step (the only reader of the world's active flag).
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_world);
			reinterpret_cast<Step_t>(vtable[0x32])(a_world, a_arg);
		}

		// Under stepLock: each remote world once, one after the other (Havok isn't built for two
		// worlds stepping at once), never the one the engine just stepped (a_stepped).
		int StepAll(RE::bhkWorld* a_stepped)
		{
			std::vector<RE::bhkWorld*> done;
			for (const auto& [cell, world] : RemoteCell::stepWorlds) {
				if (world && world != a_stepped && std::ranges::find(done, world) == done.end()) {
					Step(world, 0);
					done.push_back(world);
				}
			}
			if (!done.empty()) {
				++RemoteCell::steppedFrames;
			}
			return static_cast<int>(done.size());
		}
	}

	// The engine steps only the current space's world, in a job (ID 18870 / 18897: *ID 400440,
	// vfunc 0x32). Stepping the remote cell's world from the player update instead ran it
	// alongside that job; Havok isn't built for two worlds stepping at once, and as soon as
	// clutter in a freshly loaded cell was actually simulated (settling buckets and tankards),
	// memory got corrupted and the portal pass drew render passes that had been freed. Stepping
	// it right after the engine's world, in the same job, keeps the steps serialized.
	struct StepHook
	{
		static void Thunk(RE::bhkWorld* a_world, std::uint32_t a_arg)
		{
			Step(a_world, a_arg);
			if (!RemoteCell::stepInJob) {
				return;
			}
			std::scoped_lock lock(RemoteCell::stepLock);
			StepAll(a_world);
		}
	};

	// With Precision loaded: its hook in the world step loop (ID 77851 + 0x407) takes a lock that
	// its own main-thread update also holds; a second world stepped inside the engine's step job
	// ran into that and the game froze. At the end of the frame (Main::Update after the jobs are
	// done) nothing else is in the physics or in Precision, and the remote world is stepped there.
	nlohmann::json RemoteCell::RenderPasses(bool a_drop)
	{
		static const auto [begin, end] = [] {
			const auto& module = REL::Module::get();
			return std::pair{ module.base(), module.base() + module.segment(REL::Segment::rdata).offset() + module.segment(REL::Segment::rdata).size() };
		}();
		int shapes = 0, lists = 0, passes = 0, stale = 0, foreign = 0, dropped = 0;
		int         ownFade = 0, otherFade = 0;
		std::string sample;
		const auto check = [&](RE::BSShaderProperty::RenderPassArray& a_list, RE::BSGeometry* a_shape) {
			if (!a_list.head) {
				return;
			}
			++lists;
			bool bad = false;
			int  guard = 0;
			for (auto* pass = a_list.head; pass && guard < 64; pass = pass->next, ++guard) {
				++passes;
				const auto vtable = pass->shader ? *reinterpret_cast<const std::uintptr_t*>(pass->shader) : 0;
				if (vtable < begin || vtable >= end) {
					++stale;
					bad = true;
					break;
				}
				if (pass->geometry != a_shape) {
					++foreign;
					bad = true;
					break;
				}
			}
			if (bad && a_drop) {
				a_list.head = nullptr;
				++dropped;
			}
		};
		const auto visit = [&](RE::NiAVObject* a_root) {
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_shape) {
				++shapes;
				if (auto* shader = a_shape->GetGeometryRuntimeData().shaderProperty.get()) {
					RE::BSFadeNode* owner = nullptr;
					for (auto* up = a_shape->parent; up && !owner; up = up->parent) {
						owner = up->AsFadeNode();
					}
					if (shader->fadeNode != owner) {
						++otherFade;
						if (sample.empty()) {
							sample = std::format("'{}' fadeNode {:X} owner {:X} '{}'", a_shape->name.c_str(), reinterpret_cast<std::uintptr_t>(shader->fadeNode),
								reinterpret_cast<std::uintptr_t>(owner), owner ? owner->name.c_str() : "");
						}
					} else {
						++ownFade;
					}
					check(shader->renderPassList, a_shape);
					check(shader->debugRenderPassList, a_shape);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		};
		nlohmann::json tree = nlohmann::json::array();
		for (auto* cell : cells) {
			if (auto* cell3D = Cell3D(cell)) {
				visit(cell3D);
				std::string line = std::format("{:08X} flags {:X} parent '{}'{}:", cell->GetFormID(), cell3D->GetFlags().underlying(), cell3D->parent ? cell3D->parent->name.c_str() : "",
					LightScene::IsOurs(cell3D->parent) ? " (portal scene)" : "");
				if (auto* node = cell3D->AsNode()) {
					for (auto& child : node->GetChildren()) {
						auto* childNode = child ? child->AsNode() : nullptr;
						int below = 0;
						if (child) {
							RE::BSVisit::TraverseScenegraphGeometries(child.get(), [&](RE::BSGeometry*) {
								++below;
								return RE::BSVisit::BSVisitControl::kContinue;
							});
						}
						line += child ? std::format(" [{} {:X} n={} shapes={}]", child->name.c_str(), child->GetFlags().underlying(), childNode ? childNode->GetChildren().size() : 0, below) : " [null]";
					}
				}
				tree.push_back(line);
			}
		}
		for (auto& top : borrowedPersistent) {
			visit(top.get());
		}
		return { { "shapes", shapes }, { "lists", lists }, { "passes", passes }, { "stale", stale }, { "foreign", foreign }, { "dropped", dropped }, { "cells", tree },
			{ "ownFade", ownFade }, { "otherFade", otherFade }, { "sample", sample } };
	}

	void RemoteCell::StillLooseObjects(RE::TESObjectCELL* a_cell)
	{
		int stilled = 0, replaced = 0;
		a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			auto* object = a_ref ? a_ref->Get3D() : nullptr;
			if (!object || a_ref->As<RE::Actor>()) {
				return RE::BSContainer::ForEachResult::kContinue;  // skeletons keep their ragdoll and controller ties
			}
			bool newlyStilled = false;
			RE::BSVisit::TraverseScenegraphObjects(object, [&](RE::NiAVObject* a_object) {
				auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
				if (collision && collision->flags.any(RE::bhkNiCollisionObject::Flag::kActive)) {
					stilledClutter.emplace_back(RE::NiPointer<RE::NiAVObject>(a_object), collision->flags.underlying());
					collision->flags.reset(RE::bhkNiCollisionObject::Flag::kActive, RE::bhkNiCollisionObject::Flag::kSyncOnUpdate);
					newlyStilled = true;
					++stilled;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			// Where the item is recorded, 3D and body both (a warp moves the body too): already
			// moved by its body before it was stilled, and, handed back to the engine on walking out,
			// the body would carry it off again from wherever it was left.
			// Merged (into an interior), the 3D stands at the mapped place, never at the recorded one.
			if (newlyStilled || (!merged && object->world.translate.GetDistance(a_ref->GetPosition()) > 50.0f)) {
				a_ref->Update3DPosition(true);
				++replaced;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		if (stilled || replaced) {
			logger::info("portal: {} collision object(s) in {:08X} made visual only, {} reference(s) placed where they are recorded", stilled, a_cell->GetFormID(), replaced);
		}
	}

	void RemoteCell::CheckLooseItems()
	{
		int total = 0, with3D = 0, fallen = 0, loaded = 0, stillMissing = 0, skippedQueued = 0;
		int details = 0;
		for (auto* cell : cells) {
			auto* data = cell->GetRuntimeData().loadedData;
			// Something still queued for this cell may be these very items, which the engine is about
			// to load: loading them a second time here could leave two 3D behind.
			const bool queued = !data || data->queuedRefCount != 0 || data->criticalQueuedRefCount != 0;
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (!base || !base->IsInventoryObject() || a_ref->IsDisabled() || a_ref->IsDeleted()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				++total;
				auto*      object = a_ref->Get3D();
				const auto pos = a_ref->GetPosition();
				if (object) {
					++with3D;
					if (object->world.translate.GetDistance(pos) > 200.0f) {
						++fallen;
						if (details++ < 10) {
							logger::info("portal:   item {:08X} '{}': 3D at ({:.0f},{:.0f},{:.0f}), the item at ({:.0f},{:.0f},{:.0f})", a_ref->GetFormID(), base->GetName(),
								object->world.translate.x, object->world.translate.y, object->world.translate.z, pos.x, pos.y, pos.z);
						}
					}
				} else if (queued) {
					++skippedQueued;
				} else {
					a_ref->Load3D(false);
					a_ref->Get3D() ? ++loaded : ++stillMissing;
					if (details++ < 10) {
						logger::info("portal:   item {:08X} '{}' at ({:.0f},{:.0f},{:.0f}) had no 3D: {}", a_ref->GetFormID(), base->GetName(), pos.x, pos.y, pos.z,
							a_ref->Get3D() ? "loaded now" : "still none");
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		logger::info("portal: loose items in {}: {} total, {} with 3D ({} of them far from the item), {} loaded now, {} still without 3D, {} left to the engine's queue",
			Probe::DescribeCell(Cell()), total, with3D, fallen, loaded, stillMissing, skippedQueued);

	}

	namespace
	{
		// Whether the Havok world has something to stand on below a_pos (within 200 units).
		bool GroundBelow(RE::bhkWorld* a_world, const RE::NiPoint3& a_pos)
		{
			const float     scale = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_pos.x * scale, a_pos.y * scale, (a_pos.z + 50.0f) * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_pos.x * scale, a_pos.y * scale, (a_pos.z - 200.0f) * scale, 0.0f);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kCharController);
			RE::BSReadLockGuard lock(a_world->worldLock);
			return a_world->PickObject(pick) && pick.rayOutput.HasHit();
		}
	}

	bool RemoteCell::GroundUnderStreet()
	{
		auto* world = Cell()->GetbhkWorld();
		if (!world) {
			return false;
		}
		// Where NPCs and loose items are recorded to be: what would fall if the world were stepped.
		// Most of them, not all: one soldier standing where no ray finds ground (a wall walk, a
		// ledge) kept the whole street from ever being simulated.
		int probed = 0, grounded = 0;
		for (auto* cell : cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				const bool falls = (actor && !actor->IsDead()) || (base && base->IsInventoryObject() && !a_ref->IsDisabled() && a_ref->Get3D());
				if (falls) {
					++probed;
					grounded += GroundBelow(world, a_ref->GetPosition()) ? 1 : 0;
				}
				return probed >= 12 ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
			});
			if (probed >= 12) {
				break;
			}
		}
		const bool ground = grounded * 4 >= probed * 3;
		if (!ground) {
			logger::info("portal: ground under only {} of {} NPC(s) and item(s) in the street; not simulated yet", grounded, probed);
		}
		return ground;
	}

	void RemoteCell::StepAtEndOfFrame()
	{
		if (stepInJob) {
			return;
		}
		std::scoped_lock lock(stepLock);
		const auto       started = std::chrono::steady_clock::now();
		const int        worlds = StepAll(nullptr);
		const float      ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (ms > 4.0f) {
			logger::info("portal: stepping {} remote world(s) took {:.1f} ms", worlds, ms);
		}
	}

	void RemoteCell::InstallStepHook(bool a_enabled)
	{
		stepEnabled = a_enabled;
		// `call qword ptr [rax+0x190]` (6 bytes) at +0x18 in both jobs; rcx/edx already hold the
		// world and the argument, so a 5-byte call to the thunk plus a nop replaces it.
		for (const auto id : { 18870, 18897 }) {
			REL::Relocation<std::uintptr_t> site{ REL::ID(id), 0x18 };
			const auto*                     code = reinterpret_cast<const std::uint8_t*>(site.address());
			if (code[0] != 0xFF || code[1] != 0x90 || *reinterpret_cast<const std::uint32_t*>(code + 2) != 0x190) {
				logger::error("portal: Havok step call in ID {} not found, remote cells won't be simulated", id);
				continue;
			}
			SKSE::GetTrampoline().write_call<5>(site.address(), StepHook::Thunk);
			REL::safe_write(site.address() + 5, std::uint8_t{ 0x90 });
		}
	}

	void RemoteCell::Remap(const RE::NiTransform& a_toSource)
	{
		if (!merged) {
			return;
		}
		RestoreBounds();
		for (auto* cell : movedCells) {
			if (auto* cell3D = Cell3D(cell)) {
				movedBounds.Capture(cell3D);
				cell3D->local = a_toSource;
				RE::NiUpdateData update;
				cell3D->Update(update);
				SettleMotion(cell3D);
			}
		}
		MoveBounds(a_toSource);
	}

	// The smallest sphere around both (NiBound::Merge).
	void MergeBound(RE::NiBound& a_into, const RE::NiBound& a_other)
	{
		if (a_other.radius <= 0.0f) {
			return;
		}
		if (a_into.radius <= 0.0f) {
			a_into = a_other;
			return;
		}
		const auto  offset = a_other.center - a_into.center;
		const float distance = offset.Length();
		if (distance + a_other.radius <= a_into.radius) {
			return;
		}
		if (distance + a_into.radius <= a_other.radius) {
			a_into = a_other;
			return;
		}
		const float radius = (distance + a_into.radius + a_other.radius) * 0.5f;
		a_into.center += offset * ((radius - a_into.radius) / distance);
		a_into.radius = radius;
	}

	// By hand: NiNode's own UpdateWorldBound does nothing (the scene update merges the children's
	// bounds as it goes, and leaves fixed ones alone).
	void RecomputeBounds(RE::NiAVObject* a_root)
	{
		if (!a_root) {
			return;
		}
		if (auto* geometry = a_root->AsGeometry()) {
			const auto& model = geometry->GetModelData().modelBound;
			a_root->worldBound.center = a_root->world * model.center;
			a_root->worldBound.radius = model.radius * a_root->world.scale;
			return;
		}
		auto* node = a_root->AsNode();
		if (!node) {
			return;
		}
		RE::NiBound bound{};
		bound.center = a_root->world.translate;
		bound.radius = 0.0f;
		for (auto& child : node->GetChildren()) {
			if (child) {
				RecomputeBounds(child.get());
				MergeBound(bound, child->worldBound);
			}
		}
		a_root->worldBound = bound;
	}

	void MovedBounds::Capture(RE::NiAVObject* a_root)
	{
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			auto* node = a_object->AsMultiBoundNode();
			auto* bound = node ? node->GetRuntimeData().multiBound.get() : nullptr;
			auto* shape = bound ? bound->data.get() : nullptr;
			const auto type = shape ? shape->GetType() : 0;
			if (type == 1 || type == 2) {  // 1 axis-aligned box, 2 oriented box
				auto* box = static_cast<RE::BSMultiBoundAABB*>(shape);
				Saved saved{ RE::NiPointer<RE::BSMultiBoundShape>(shape), box->center, box->size, {} };
				if (type == 2) {
					saved.rotation = static_cast<RE::BSMultiBoundOBB*>(shape)->rotation;
				}
				moved.push_back(std::move(saved));
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}

	void MovedBounds::Move(RE::NiAVObject* a_root, const RE::NiTransform& a_move)
	{
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			if (a_object->GetFlags().any(RE::NiAVObject::Flag::kFixedBound)) {
				a_object->GetFlags().reset(RE::NiAVObject::Flag::kFixedBound);
				fixed.emplace_back(a_object);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		RE::NiUpdateData update;
		a_root->Update(update);
		RecomputeBounds(a_root);
		// The scene update puts a cell's box around its contents itself. Captured after an update
		// and moved again, cells' boxes were saved already in the interior's coordinates and put
		// back like that: the street's ground in front of the Bannered Mare was culled from some
		// angles. Boxes it leaves where they were (an interior's rooms) are moved by hand.
		const auto& r = a_move.rotate;
		for (std::size_t i = unmoved; i < moved.size(); ++i) {
			const auto& saved = moved[i];
			auto*       box = static_cast<RE::BSMultiBoundAABB*>(saved.shape.get());
			if (box->center.GetDistance(saved.center) > 0.5f) {
				continue;
			}
			box->center = a_move * saved.center;
			if (saved.shape->GetType() == 2) {
				static_cast<RE::BSMultiBoundOBB*>(saved.shape.get())->rotation = r * saved.rotation;
			} else {
				// An axis-aligned box can't turn: the one around the turned box.
				const auto& sz = saved.size;
				for (int k = 0; k < 3; ++k) {
					box->size[k] = std::abs(r.entry[k][0]) * sz.x + std::abs(r.entry[k][1]) * sz.y + std::abs(r.entry[k][2]) * sz.z;
				}
			}
		}
		unmoved = moved.size();
	}

	void MovedBounds::Unfix(RE::NiAVObject* a_root)
	{
		bool any = false;
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			if (a_object->GetFlags().any(RE::NiAVObject::Flag::kFixedBound)) {
				a_object->GetFlags().reset(RE::NiAVObject::Flag::kFixedBound);
				fixed.emplace_back(a_object);
				any = true;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		if (any) {
			RecomputeBounds(a_root);
		}
	}

	void MovedBounds::Restore()
	{
		for (auto& object : fixed) {
			object->GetFlags().set(RE::NiAVObject::Flag::kFixedBound);
		}
		fixed.clear();
		for (auto& saved : moved) {
			auto* box = static_cast<RE::BSMultiBoundAABB*>(saved.shape.get());
			box->center = saved.center;
			box->size = saved.size;
			if (saved.shape->GetType() == 2) {
				static_cast<RE::BSMultiBoundOBB*>(saved.shape.get())->rotation = saved.rotation;
			}
		}
		moved.clear();
		unmoved = 0;
	}

	void RemoteCell::MoveBounds(const RE::NiTransform& a_toSource)
	{
		for (auto* cell : movedCells) {
			if (auto* cell3D = Cell3D(cell)) {
				movedBounds.Move(cell3D, a_toSource);
			}
		}
		if (movedBounds.Multibounds()) {
			logger::info("portal: {} multibound(s) moved with the merged cells", movedBounds.Multibounds());
		}
		if (movedBounds.Fixed()) {
			logger::info("portal: {} fixed bound(s) of the merged cells recomputed", movedBounds.Fixed());
		}
	}

	void RemoteCell::UnfixNewBounds()
	{
		for (auto* cell : cells) {
			UnfixNewBounds(cell);
		}
	}

	void RemoteCell::UnfixNewBounds(const RE::TESObjectCELL* a_cell)
	{
		if (!merged) {
			return;
		}
		if (auto* cell3D = Cell3D(a_cell)) {
			movedBounds.Unfix(cell3D);
		}
	}

	void RemoteCell::RestoreBounds()
	{
		movedBounds.Restore();
	}

	namespace
	{
		// A rotation matrix (column vectors, as NiMatrix3 and Havok both use) as Havok's quaternion
		// (x, y, z, w).
		RE::hkQuaternion ToQuaternion(const float a_m[3][3])
		{
			float      x, y, z, w;
			const float trace = a_m[0][0] + a_m[1][1] + a_m[2][2];
			if (trace > 0.0f) {
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				w = 0.25f * s;
				x = (a_m[2][1] - a_m[1][2]) / s;
				y = (a_m[0][2] - a_m[2][0]) / s;
				z = (a_m[1][0] - a_m[0][1]) / s;
			} else if (a_m[0][0] > a_m[1][1] && a_m[0][0] > a_m[2][2]) {
				const float s = std::sqrt(1.0f + a_m[0][0] - a_m[1][1] - a_m[2][2]) * 2.0f;
				w = (a_m[2][1] - a_m[1][2]) / s;
				x = 0.25f * s;
				y = (a_m[0][1] + a_m[1][0]) / s;
				z = (a_m[0][2] + a_m[2][0]) / s;
			} else if (a_m[1][1] > a_m[2][2]) {
				const float s = std::sqrt(1.0f + a_m[1][1] - a_m[0][0] - a_m[2][2]) * 2.0f;
				w = (a_m[0][2] - a_m[2][0]) / s;
				x = (a_m[0][1] + a_m[1][0]) / s;
				y = 0.25f * s;
				z = (a_m[1][2] + a_m[2][1]) / s;
			} else {
				const float s = std::sqrt(1.0f + a_m[2][2] - a_m[0][0] - a_m[1][1]) * 2.0f;
				w = (a_m[1][0] - a_m[0][1]) / s;
				x = (a_m[0][2] + a_m[2][0]) / s;
				y = (a_m[1][2] + a_m[2][1]) / s;
				z = 0.25f * s;
			}
			RE::hkQuaternion q;
			q.vec = RE::hkVector4(x, y, z, w);
			return q;
		}
	}

	namespace
	{
		// a_copy put where a_original is, carried through a_map (one space's coordinates into the other's).
		void PlaceMapped(RE::bhkRigidBody* a_copy, RE::bhkRigidBody* a_original, const RE::NiTransform& a_map)
		{
			const float     scale = RE::bhkWorld::GetWorldScale();
			RE::hkTransform original;
			a_original->GetTransform(original);
			const RE::hkVector4* columns[3] = { &original.rotation.col0, &original.rotation.col1, &original.rotation.col2 };
			float                rotated[3][3];
			for (int column = 0; column < 3; ++column) {
				const auto& c = columns[column]->quad.m128_f32;
				for (int row = 0; row < 3; ++row) {
					rotated[row][column] = a_map.rotate.entry[row][0] * c[0] + a_map.rotate.entry[row][1] * c[1] + a_map.rotate.entry[row][2] * c[2];
				}
			}
			const auto&        t = original.translation.quad.m128_f32;
			const RE::NiPoint3 at = a_map.rotate * RE::NiPoint3{ t[0], t[1], t[2] } + a_map.translate * scale;
			RE::hkVector4      position(at.x, at.y, at.z, 0.0f);
			auto               rotation = ToQuaternion(rotated);
			a_copy->SetPositionAndRotation(position, rotation);
		}

		// A body of a skeleton's bones to stand in for its owner: the spine's, or any.
		RE::bhkRigidBody* BoneBody(RE::NiAVObject* a_root)
		{
			RE::bhkRigidBody* any = nullptr;
			RE::bhkRigidBody* spine = nullptr;
			if (!a_root) {
				return nullptr;
			}
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
				auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
				auto* body = collision && collision->body ? collision->body->AsBhkRigidBody() : nullptr;
				if (!body || !body->GetRigidBody()) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				any = any ? any : body;
				if (std::string_view(a_object->name.c_str()).find("Spine") != std::string_view::npos) {
					spine = body;
					return RE::BSVisit::BSVisitControl::kStop;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return spine ? spine : any;
		}

		// A copy of a bone's body in a_world, which character controllers there run into like into
		// a door: only the layer, none of the skeleton's own filter groups.
		RemoteCell::StandIn MakeStandIn(RE::bhkRigidBody* a_original, RE::bhkWorld* a_world, const RE::NiTransform& a_map)
		{
			RE::NiPointer<RE::bhkRigidBody> copy(static_cast<RE::bhkRigidBody*>(a_original->Clone()));
			auto*                           rigid = copy ? copy->GetRigidBody() : nullptr;
			if (!rigid) {
				return {};
			}
			const auto motion = rigid->motion.type.get();
			if (motion != RE::hkpMotion::MotionType::kKeyframed && motion != RE::hkpMotion::MotionType::kFixed) {
				return {};  // a body of its own would fall
			}
			*reinterpret_cast<std::uint32_t*>(&rigid->collidable.broadPhaseHandle.collisionFilterInfo) = static_cast<std::uint32_t>(RE::COL_LAYER::kAnimStatic);
			PlaceMapped(copy.get(), a_original, a_map);
			copy->MoveToWorld(a_world);
			return { copy, RE::NiPointer<RE::bhkRigidBody>(a_original) };
		}
	}

	void RemoteCell::UpdateStandIns(const RE::NiTransform& a_toSource, RE::bhkWorld* a_world)
	{
		auto* roomWorld = Cell() ? Cell()->GetbhkWorld() : nullptr;
		if (!roomWorld || !a_world) {
			return;
		}
		// The room's people stand in the room moved to its door (Merge), and their bones' bodies,
		// driven from the bones, are where they are seen: in the player's world's coordinates
		// already. The player's body is mapped into the room's.
		const RE::NiTransform          alreadyThere;
		std::unordered_set<RE::FormID> present;
		for (auto* cell : cells) {
			for (auto* actor : ActorsIn(cell)) {
				auto* body = actor->IsDead() ? nullptr : BoneBody(actor->Get3D());
				if (!body) {
					continue;
				}
				present.insert(actor->GetFormID());
				auto& standIn = actorStandIns[actor->GetFormID()];
				if (standIn.original.get() != body) {
					// New, or the actor's 3D was built anew.
					if (standIn.copy) {
						standIn.copy->RemoveFromCurrentWorld();
					}
					standIn = MakeStandIn(body, a_world, alreadyThere);
					standIn.original.reset(body);  // also when none could be made: not tried again every frame
					const auto* rigid = body->GetRigidBody();
					RE::hkTransform from, to;
					body->GetTransform(from);
					if (standIn.copy) {
						standIn.copy->GetTransform(to);
					}
					const float inv = 1.0f / RE::bhkWorld::GetWorldScale();
					logger::info("portal: stand-in for {:08X} '{}' in the world: {} (bone body motion {}, layer {}) at ({:.0f},{:.0f},{:.0f}) -> ({:.0f},{:.0f},{:.0f}), actor at ({:.0f},{:.0f},{:.0f})",
						actor->GetFormID(), actor->GetDisplayFullName(), standIn.copy ? "made" : "none", rigid ? static_cast<int>(rigid->motion.type.get()) : -1,
						rigid ? static_cast<int>(rigid->collidable.broadPhaseHandle.collisionFilterInfo.GetCollisionLayer()) : -1, from.translation.quad.m128_f32[0] * inv,
						from.translation.quad.m128_f32[1] * inv, from.translation.quad.m128_f32[2] * inv, to.translation.quad.m128_f32[0] * inv, to.translation.quad.m128_f32[1] * inv,
						to.translation.quad.m128_f32[2] * inv, actor->GetPositionX(), actor->GetPositionY(), actor->GetPositionZ());
				} else if (standIn.copy) {
					PlaceMapped(standIn.copy.get(), body, alreadyThere);
				}
			}
		}
		std::erase_if(actorStandIns, [&](auto& a_entry) {
			auto& [id, standIn] = a_entry;
			if (present.contains(id)) {
				return false;
			}
			if (standIn.copy) {
				standIn.copy->RemoveFromCurrentWorld();
			}
			return true;
		});
		auto*      body = BoneBody(RE::PlayerCharacter::GetSingleton()->Get3D(false));
		const auto toRoom = a_toSource.Invert();
		if (body && playerStandIn.original.get() == body && playerStandIn.copy) {
			PlaceMapped(playerStandIn.copy.get(), body, toRoom);
		} else {
			if (playerStandIn.copy) {
				playerStandIn.copy->RemoveFromCurrentWorld();
			}
			playerStandIn = body ? MakeStandIn(body, roomWorld, toRoom) : StandIn{};
		}
	}

	void RemoteCell::ClearStandIns()
	{
		for (auto& [id, standIn] : actorStandIns) {
			if (standIn.copy) {
				standIn.copy->RemoveFromCurrentWorld();
			}
		}
		actorStandIns.clear();
		if (playerStandIn.copy) {
			playerStandIn.copy->RemoveFromCurrentWorld();
		}
		playerStandIn = {};
	}

	void RemoteCell::MergeCollision(const RE::NiTransform& a_toSource, RE::bhkWorld* a_world, RE::FormID a_skip, const RE::NiPoint3& a_doorCenter, const RE::NiPoint3& a_inward)
	{
		SEAMLESS_ASSERT(a_world);
		UnmergeCollision();
		int         skipped = 0, inWorld = 0, behindDoor = 0;
		mergedWorld = a_world;
		const float scale = RE::bhkWorld::GetWorldScale();
		for (auto* cell : cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* object = a_ref ? a_ref->Get3D() : nullptr;
				if (!object || a_ref->As<RE::Actor>() || a_ref->GetFormID() == a_skip || a_ref->IsDisabled()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				RE::BSVisit::TraverseScenegraphObjects(object, [&](RE::NiAVObject* a_object) {
					auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
					auto* body = collision && collision->body ? collision->body->AsBhkRigidBody() : nullptr;
					auto* rigid = body ? body->GetRigidBody() : nullptr;
					if (!rigid) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// Only what stays put. Loose things are stilled while merged (see Merge) and
					// would need a body of their own that moves back with them.
					const auto motion = rigid->motion.type.get();
					if (motion != RE::hkpMotion::MotionType::kFixed && motion != RE::hkpMotion::MotionType::kKeyframed) {
						++skipped;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					bool straddles = false;  // across the doorway, see SetDoorwayPiecesPresent
					// Behind the room's door, where the outdoors is when merged, rooms have a box with a
					// closed door painted in it (what the opened door would show). Copied, it stood
					// just outside the house's door: walking out of the room, the player stopped there.
					{
						RE::hkAabb box;
						body->GetAabbWorldspace(box);
						float              deepest = -FLT_MAX, shallowest = FLT_MAX, left = FLT_MAX, right = -FLT_MAX;
						const RE::NiPoint3 across{ a_inward.y, -a_inward.x, 0.0f };
						for (int corner = 0; corner < 8; ++corner) {
							const RE::NiPoint3 at{ ((corner & 1) ? box.max : box.min).quad.m128_f32[0] / scale, ((corner & 2) ? box.max : box.min).quad.m128_f32[1] / scale,
								((corner & 4) ? box.max : box.min).quad.m128_f32[2] / scale };
							deepest = std::max(deepest, (at - a_doorCenter).Dot(a_inward));
							shallowest = std::min(shallowest, (at - a_doorCenter).Dot(a_inward));
							left = std::min(left, (at - a_doorCenter).Dot(across));
							right = std::max(right, (at - a_doorCenter).Dot(across));
						}
						if (deepest < 1.0f) {
							++behindDoor;
							return RE::BSVisit::BSVisitControl::kContinue;
						}
						straddles = shallowest < -1.0f && left < 0.0f && right > 0.0f;
					}
					RE::NiPointer<RE::bhkRigidBody> copy(static_cast<RE::bhkRigidBody*>(body->Clone()));
					SEAMLESS_ASSERT(copy && copy->GetRigidBody());
					PlaceMapped(copy.get(), body, a_toSource);
					copy->MoveToWorld(a_world);
					inWorld += copy->GetWorld1() == a_world->GetWorld1() ? 1 : 0;
					if (straddles) {
						doorwayPieces.push_back(copy);
					}
					mergedBodies.push_back(std::move(copy));
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		logger::info("portal: {} collision bodies of the merged room copied into the world, {} of them in it, {} across its doorway ({} loose ones and {} behind its door left out)", mergedBodies.size(), inWorld, doorwayPieces.size(), skipped, behindDoor);
	}

	void RemoteCell::SetDoorwayPiecesPresent(bool a_present)
	{
		if (a_present == doorwayPiecesPresent || !mergedWorld) {
			return;
		}
		doorwayPiecesPresent = a_present;
		for (auto& body : doorwayPieces) {
			if (a_present) {
				body->MoveToWorld(mergedWorld);
			} else {
				body->RemoveFromCurrentWorld();
			}
		}
	}

	void RemoteCell::UnmergeCollision()
	{
		ClearStandIns();
		for (auto& body : mergedBodies) {
			body->RemoveFromCurrentWorld();
		}
		mergedBodies.clear();
		doorwayPieces.clear();
		doorwayPiecesPresent = true;
		mergedWorld = nullptr;
	}

	json RemoteCell::Merge(const RE::NiTransform& a_toSource)
	{
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		auto* here3D = Cell3D(here);
		auto* worldParent = here3D ? here3D->parent : nullptr;
		if (!worldParent || cells.empty()) {
			throw std::runtime_error("nothing to merge");
		}
		auto* root = scene.get();
		int   moved = 0;
		RestoreBounds();
		movedCells.clear();
		for (auto* cell : cells) {
			RE::NiPointer<RE::NiAVObject> cell3D(Cell3D(cell));
			if (cell3D && cell3D->parent == root) {
				movedCells.push_back(cell);
				movedBounds.Capture(cell3D.get());
				root->DetachChild(cell3D.get());
				worldParent->AttachChild(cell3D.get(), true);
				cell3D->local = a_toSource;
				// Loose objects (a mace on a shelf, cheese, plates) take their place from their
				// physics body, in the cell's own coordinates: they stayed behind, tens of thousands
				// of units away, and the shelves of the merged room were empty. While merged their
				// collision objects don't drive the 3D, which then follows the cell like the rest.
				RE::BSVisit::TraverseScenegraphObjects(cell3D.get(), [&](RE::NiAVObject* a_object) {
					auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
					bool ofActor = false;
					for (auto* up = a_object; collision && up && up != cell3D.get(); up = up->parent) {
						if (auto* owner = up->GetUserData(); owner && owner->Is(RE::FormType::ActorCharacter)) {
							ofActor = true;  // skeletons keep their ragdoll and controller ties
							break;
						}
					}
					if (collision && !ofActor && collision->flags.any(RE::bhkNiCollisionObject::Flag::kActive)) {
						stilledClutter.emplace_back(RE::NiPointer<RE::NiAVObject>(a_object), collision->flags.underlying());
						collision->flags.reset(RE::bhkNiCollisionObject::Flag::kActive, RE::bhkNiCollisionObject::Flag::kSyncOnUpdate);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				RE::NiUpdateData update;
				cell3D->Update(update);
				SettleMotion(cell3D.get());
				++moved;
			}
		}
		MoveBounds(a_toSource);
		// The room's lights stay in the portal's light scene: the patched Community Shaders lights the
		// merged shapes from there (SetupPortalStrictLights). Handed to the world's, they lit the
		// street around the door, through walls, took shadow slots from the town's own lights (its
		// shadows changed as doors were passed) and stayed behind after the room was let go.
		merged = true;
		return { { "cells", moved }, { "parent", worldParent->name.c_str() } };
	}

	json RemoteCell::Describe() const
	{
		auto* cell = Cell();
		if (!cell) {
			return nullptr;
		}
		json actors = json::array();
		for (auto* actor : ActorsIn(cell)) {
			const auto pos = actor->GetPosition();
			actors.push_back({
				{ "name", actor->GetDisplayFullName() },
				{ "has3D", actor->Get3D() != nullptr },
				{ "pos", { pos.x, pos.y, pos.z } },
			});
		}
		return {
			{ "cell", Probe::DescribeCell(cell) },
			{ "neighbours", cells.size() - 1 },
			{ "healthy", Healthy() },
			{ "fullyLoaded", FullyLoaded() },
			{ "loadState", [cell] {
				 auto* loaded = cell->GetRuntimeData().loadedData;
				 return loaded ? json{ { "refsFullyLoaded", loaded->refsFullyLoaded }, { "queued", loaded->queuedRefCount }, { "criticalQueued", loaded->criticalQueuedRefCount }, { "distantQueued", loaded->queuedDistantRefCount } } : json(nullptr);
			 }() },
			{ "adoptedLights", adoptedLights },
			{ "fade", [this] {
				 // Diagnostics: BSFadeNodes by current fade (0 = invisible, 1 = fully shown).
				 std::uint32_t hidden = 0, partial = 0, shown = 0;
				 std::function<void(RE::NiAVObject*)> visit = [&](RE::NiAVObject* a_object) {
					 if (!a_object) {
						 return;
					 }
					 if (auto* fade = a_object->AsFadeNode()) {
						 const float value = fade->GetRuntimeData().currentFade;
						 (value <= 0.0f ? hidden : value >= 1.0f ? shown : partial)++;
					 }
					 if (auto* node = a_object->AsNode()) {
						 for (auto& child : node->GetChildren()) {
							 visit(child.get());
						 }
					 }
				 };
				 visit(Cell3D());
				 return json{ { "hidden", hidden }, { "partial", partial }, { "shown", shown } };
			 }() },
			{ "steppedFrames", steppedFrames.load() },
			{ "actors", actors },
		};
	}

	namespace
	{
		// The change into an exterior (ID 13317) re-uses the grid buffered from before only if
		// every one of its cells is merely loaded (ID 15951: state kLoaded); otherwise it loads the
		// grid anew and then sleeps until the loader's queue is empty (ID 75708, 50 ms at a time).
		// Cells handed over still attached failed that check, and the walk out took longer than
		// with them detached and attached again. They count as ready here; the attach loop that
		// follows skips them (it attaches unattached cells only).
		struct GridReadyHook
		{
			static bool Thunk(void* a_grid)
			{
				if (func(a_grid)) {
					return true;
				}
				if (RemoteCell::HandedOver().empty() || !a_grid) {
					return false;
				}
				const auto* raw = static_cast<const std::uint8_t*>(a_grid);
				const auto  size = *reinterpret_cast<const std::uint32_t*>(raw + 0x10);
				auto* const* gridCells = *reinterpret_cast<RE::TESObjectCELL* const* const*>(raw + 0x18);
				int          accepted = 0;
				for (std::uint32_t i = 0; gridCells && i < size * size; ++i) {
					auto* cell = gridCells[i];
					const auto state = cell ? cell->cellState.get() : CellState::kUnloaded;
					if (state == CellState::kUnattached) {
						continue;
					}
					if (state != CellState::kAttached || std::ranges::find(RemoteCell::HandedOver(), cell) == RemoteCell::HandedOver().end()) {
						auto* coordinates = cell ? cell->GetCoordinates() : nullptr;
						logger::info("portal: grid not ready: cell ({},{}) in state {}{}", coordinates ? coordinates->cellX : 0, coordinates ? coordinates->cellY : 0,
							static_cast<int>(state), cell ? "" : " (none)");
						return false;
					}
					++accepted;
				}
				logger::info("portal: grid ready with {} cell(s) handed over attached", accepted);
				return true;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	namespace
	{
		// Detaching an interior (ours on release, or the engine's on a change of space) puts it at
		// the front of the engine's buffer of recent interiors (ID 13435, TES::interiorBuffer) and
		// unloads the one that falls off the end (ID 13721). That can be a cell we still hold
		// attached: its 3D was detached from objRoot by the index it has under our root (0), which
		// took objRoot's first child, the temporary node manager, and the game crashed the next
		// frame in its update (ID 13292); a merged room vanished from the street. A held cell is
		// left loaded and only drops out of the buffer; its own release puts it back in.
		struct EvictInteriorHook
		{
			static void Thunk(RE::TESDataHandler* a_data, RE::TESObjectCELL* a_cell)
			{
				if (RemoteCell::Holds(a_cell)) {
					logger::info("portal: {} kept loaded, out of the interior buffer (held behind a door)", Probe::DescribeCell(a_cell));
					return;
				}
				func(a_data, a_cell);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	bool RemoteCell::Holds(const RE::TESObjectCELL* a_cell)
	{
		return a_cell && std::ranges::find(held, a_cell) != held.end();
	}

	void RemoteCell::InstallGridHook()
	{
		REL::Relocation<std::uintptr_t> evict{ REL::ID(13435), 0x7C };
		if (Hooks::CallsTo(evict.address(), 13721, "portal: interior buffer eviction")) {
			EvictInteriorHook::func = SKSE::GetTrampoline().write_call<5>(evict.address(), EvictInteriorHook::Thunk);
		}
		REL::Relocation<std::uintptr_t> site{ REL::ID(13317), 0xF4 };
		if (!Hooks::CallsTo(site.address(), 15951, "portal: grid ready check")) {
			logger::warn("portal: street cells are detached when walking out");
			handOverStreet = false;
			return;
		}
		GridReadyHook::func = SKSE::GetTrampoline().write_call<5>(site.address(), GridReadyHook::Thunk);
	}
}
