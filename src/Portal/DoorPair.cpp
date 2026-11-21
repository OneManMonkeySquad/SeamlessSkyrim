#include "Portal/DoorPair.h"

#include "Paths.h"

namespace Portal::Doors
{
	RE::TESObjectREFR* Door(RE::FormID a_id)
	{
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
		if (!door || !door->Get3D()) {
			throw std::runtime_error(std::format("door {:08X} has no 3D", a_id));
		}
		return door;
	}

	// ---- the door's own geometry -------------------------------------------------------------
	// The base object's bounds (OBND) are no measure of the panel: a load door's mesh also holds a
	// box behind the panel that hides the void behind a closed door ("DoorToBlack"), handles,
	// sometimes a frame or a piece of wall. The shack door of Whiterun's guard house is 51 units
	// deep for an 8 unit panel; centred on those bounds, every doorway was mapped 20-30 units off
	// (both doors of a pair carry the box on opposite sides) and the portal stood that far in
	// front of the door. The panel is measured from the mesh instead: the leaves are the nodes the
	// door's own opening animation turns, and its faces are where most of its surface faces the
	// doorway (a handle or the box's side walls hardly do).
	namespace
	{
		// Every caller is on the main thread today (player update, render hooks, the menu, bridge
		// commands run as main-thread tasks); the swung leaves are read while measuring.
		// Recursive: Swing measures and reads under the same lock.
		std::recursive_mutex geometryLock;

		// Doors swung open (Swing), with the transforms their leaves had closed.
		struct SwungLeaf
		{
			RE::NiPointer<RE::NiAVObject> node;
			RE::NiTransform               closedLocal;
		};
		std::unordered_map<RE::FormID, std::vector<SwungLeaf>> swungClosed;

		const RE::NiTransform* ClosedLocal(RE::FormID a_door, const RE::NiAVObject* a_node)
		{
			const auto it = swungClosed.find(a_door);
			if (it != swungClosed.end()) {
				for (const auto& leaf : it->second) {
					if (leaf.node.get() == a_node) {
						return &leaf.closedLocal;
					}
				}
			}
			return nullptr;
		}

		// a_object's transform in the door's model space (its 3D root's frame), with the door closed.
		RE::NiTransform ToRoot(RE::FormID a_door, const RE::NiAVObject* a_object, const RE::NiAVObject* a_root)
		{
			RE::NiTransform result;
			for (auto* node = a_object; node && node != a_root; node = node->parent) {
				const auto* closed = ClosedLocal(a_door, node);
				result = (closed ? *closed : node->local) * result;
			}
			return result;
		}

		// The nodes the door's opening animation moves (NiMultiTargetTransformController targets),
		// outermost only: a handle animated on its panel moves with it.
		std::vector<RE::NiAVObject*> AnimatedNodes(RE::NiAVObject* a_root)
		{
			std::vector<RE::NiAVObject*> nodes;
			for (auto* controller = a_root->GetControllers(); controller; controller = controller->next.get()) {
				auto* multi = netimmerse_cast<RE::NiMultiTargetTransformController*>(controller);
				for (std::uint16_t i = 0; multi && multi->targets && i < multi->numInterps; ++i) {
					auto* target = multi->targets[i];
					if (target && target != a_root && std::ranges::find(nodes, target) == nodes.end()) {
						nodes.push_back(target);
					}
				}
			}
			std::erase_if(nodes, [&](RE::NiAVObject* a_node) {
				for (auto* parent = a_node->parent; parent && parent != a_root; parent = parent->parent) {
					if (std::ranges::find(nodes, parent) != nodes.end()) {
						return true;
					}
				}
				return false;
			});
			return nodes;
		}

		// a_visit(a, b, c) for each triangle of the shape, in the door's model space. False when the
		// shape keeps no copy of its mesh in memory.
		template <class F>
		bool ForEachTriangle(RE::BSGeometry* a_shape, const RE::NiTransform& a_toRoot, F&& a_visit)
		{
			auto* triShape = netimmerse_cast<RE::BSTriShape*>(a_shape);
			auto& data = a_shape->GetGeometryRuntimeData();
			auto* mesh = data.rendererData;
			if (!triShape || !mesh || !mesh->rawVertexData || !mesh->rawIndexData) {
				return false;
			}
			const auto          desc = *reinterpret_cast<const std::uint64_t*>(&data.vertexDesc);
			const std::uint32_t stride = static_cast<std::uint32_t>(desc & 0xF) * 4;  // positions first, as floats
			const auto&         counts = triShape->GetTrishapeRuntimeData();
			if (stride < 12) {
				return false;
			}
			const auto vertex = [&](std::uint16_t a_index) {
				const auto* p = reinterpret_cast<const float*>(mesh->rawVertexData + static_cast<std::size_t>(a_index) * stride);
				return a_toRoot * RE::NiPoint3{ p[0], p[1], p[2] };
			};
			for (std::uint32_t t = 0; t < counts.triangleCount; ++t) {
				const auto* index = mesh->rawIndexData + static_cast<std::size_t>(t) * 3;
				if (index[0] < counts.vertexCount && index[1] < counts.vertexCount && index[2] < counts.vertexCount) {
					a_visit(vertex(index[0]), vertex(index[1]), vertex(index[2]));
				}
			}
			return true;
		}

		struct Box
		{
			RE::NiPoint3 min{ FLT_MAX, FLT_MAX, FLT_MAX };
			RE::NiPoint3 max{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

			void Add(const RE::NiPoint3& a_point)
			{
				min = { std::min(min.x, a_point.x), std::min(min.y, a_point.y), std::min(min.z, a_point.z) };
				max = { std::max(max.x, a_point.x), std::max(max.y, a_point.y), std::max(max.z, a_point.z) };
			}
			[[nodiscard]] bool Empty() const { return min.x > max.x; }
		};

		struct Leaf
		{
			RE::NiPointer<RE::NiAVObject> node;
			Box                           box;    // its shapes, in the door's model space
			RE::NiPoint3                  pivot;  // its origin there: what its animation turns it around
		};

		struct Geometry
		{
			RE::NiPointer<RE::NiAVObject> root;  // the 3D measured; a reloaded door is measured anew
			Box                           panel;  // the leaves; along the thin axis, their faces
			bool                          thinX = false;
			bool                          measured = false;  // false: the base object's bounds
			std::vector<Leaf>             leaves;  // the root itself when nothing is animated
			// Shapes the leaves don't move, with their extent: a frame stays, the box behind the
			// panel goes with the panel (Open).
			std::vector<std::pair<RE::NiPointer<RE::BSGeometry>, Box>> fixed;
		};

		std::unordered_map<RE::FormID, Geometry>   geometries;  // under geometryLock

		Geometry Measure(RE::TESObjectREFR* a_door, RE::NiAVObject* a_root)
		{
			const auto id = a_door->GetFormID();
			Geometry   geometry;
			geometry.root.reset(a_root);
			const auto animated = AnimatedNodes(a_root);
			const auto leafOf = [&](const RE::NiAVObject* a_object) -> int {
				for (auto* node = a_object; node && node != a_root; node = node->parent) {
					if (const auto it = std::ranges::find(animated, node); it != animated.end()) {
						return static_cast<int>(it - animated.begin());
					}
				}
				return animated.empty() ? 0 : -1;
			};
			if (animated.empty()) {
				geometry.leaves.push_back({ RE::NiPointer<RE::NiAVObject>(a_root), {}, {} });
			}
			for (auto* node : animated) {
				geometry.leaves.push_back({ RE::NiPointer<RE::NiAVObject>(node), {}, ToRoot(id, node, a_root).translate });
			}

			struct Triangle
			{
				RE::NiPoint3 a, b, c;
			};
			std::vector<Triangle> panelTriangles;
			bool                  complete = true;
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_shape) {
				const int  leaf = leafOf(a_shape);
				const auto toRoot = ToRoot(id, a_shape, a_root);
				Box        box;
				complete &= ForEachTriangle(a_shape, toRoot, [&](const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c) {
					box.Add(a);
					box.Add(b);
					box.Add(c);
					if (leaf >= 0) {
						panelTriangles.push_back({ a, b, c });
					}
				});
				if (box.Empty()) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				if (leaf >= 0) {
					auto& leafBox = geometry.leaves[leaf].box;
					leafBox.Add(box.min);
					leafBox.Add(box.max);
				} else {
					geometry.fixed.emplace_back(RE::NiPointer<RE::BSGeometry>(a_shape), box);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			for (const auto& leaf : geometry.leaves) {
				if (!leaf.box.Empty()) {
					geometry.panel.Add(leaf.box.min);
					geometry.panel.Add(leaf.box.max);
				}
			}
			if (!complete || geometry.panel.Empty()) {
				const auto& bound = a_door->GetBaseObject()->boundData;
				geometry.panel = {};
				geometry.panel.Add({ static_cast<float>(bound.boundMin.x), static_cast<float>(bound.boundMin.y), static_cast<float>(bound.boundMin.z) });
				geometry.panel.Add({ static_cast<float>(bound.boundMax.x), static_cast<float>(bound.boundMax.y), static_cast<float>(bound.boundMax.z) });
				geometry.thinX = (bound.boundMax.x - bound.boundMin.x) < (bound.boundMax.y - bound.boundMin.y);
				for (auto& leaf : geometry.leaves) {
					leaf.box = geometry.panel;
				}
				geometry.fixed.clear();
				logger::info("portal: door {:08X}: mesh not readable, using its bounds", id);
				return geometry;
			}
			geometry.measured = true;
			geometry.thinX = (geometry.panel.max.x - geometry.panel.min.x) < (geometry.panel.max.y - geometry.panel.min.y);

			// The faces: surface facing along the thin axis, by where it is on that axis (1 unit
			// steps); the outermost steps holding a good share of the most are the panel's faces.
			std::map<int, float> facing;
			for (const auto& t : panelTriangles) {
				const auto  normal = (t.b - t.a).Cross(t.c - t.a);  // length: twice the area
				const float along = 0.5f * std::abs(geometry.thinX ? normal.x : normal.y);
				const float at = (geometry.thinX ? t.a.x + t.b.x + t.c.x : t.a.y + t.b.y + t.c.y) / 3.0f;
				facing[static_cast<int>(std::floor(at))] += along;
			}
			float most = 0.0f;
			for (const auto& [at, area] : facing) {
				most = std::max(most, area);
			}
			int front = INT_MAX, back = INT_MIN;
			for (const auto& [at, area] : facing) {
				if (area >= 0.25f * most) {
					front = std::min(front, at);
					back = std::max(back, at);
				}
			}
			if (front <= back) {
				auto& low = geometry.thinX ? geometry.panel.min.x : geometry.panel.min.y;
				auto& high = geometry.thinX ? geometry.panel.max.x : geometry.panel.max.y;
				low = std::max(low, static_cast<float>(front));
				high = std::min(high, static_cast<float>(back + 1));
			}
			logger::info("portal: door {:08X}: panel ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}), {} leaf/leaves, {} fixed shape(s)", id,
				geometry.panel.min.x, geometry.panel.min.y, geometry.panel.min.z, geometry.panel.max.x, geometry.panel.max.y, geometry.panel.max.z,
				geometry.leaves.size(), geometry.fixed.size());
			return geometry;
		}

		// The door's geometry, measured the first time it's asked for with this 3D.
		Geometry& GeometryOf(RE::TESObjectREFR* a_door)
		{
			auto* root = a_door->Get3D();
			auto& geometry = geometries[a_door->GetFormID()];
			if (geometry.root.get() != root) {
				geometry = Measure(a_door, root);
			}
			return geometry;
		}
	}

	// The door's world transform as if it were closed.
	RE::NiTransform ClosedWorld(RE::TESObjectREFR* a_door, RE::NiAVObject* a_door3D)
	{
		std::scoped_lock lock(geometryLock);
		const auto*      closed = ClosedLocal(a_door->GetFormID(), a_door3D);
		if (!closed) {
			return a_door3D->world;
		}
		return a_door3D->parent ? a_door3D->parent->world * *closed : *closed;
	}

	// Where the door stands in its own cell. A cell merged into another space (render-merge) has
	// its whole 3D moved, and the door's world transform with it; the door pair maps between the
	// cells' own coordinates.
	// Its cell's 3D, when the door hangs under it (it may not: a persistent door's 3D borrowed
	// elsewhere).
	RE::NiAVObject* CellNodeAbove(RE::TESObjectREFR* a_door, RE::NiAVObject* a_door3D)
	{
		auto* cell = a_door->GetParentCell();
		auto* loaded = cell ? cell->GetRuntimeData().loadedData : nullptr;
		auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
		for (auto* node = a_door3D->parent; cell3D && node; node = node->parent) {
			if (node == cell3D) {
				return cell3D;
			}
		}
		return nullptr;
	}

	RE::NiTransform Placement(RE::TESObjectREFR* a_door)
	{
		auto*      door3D = a_door->Get3D();
		const auto closed = ClosedWorld(a_door, door3D);
		auto*      cell3D = CellNodeAbove(a_door, door3D);
		return cell3D ? cell3D->world.Invert() * closed : closed;
	}

	// The cell's own coordinates to where it is drawn: Placement undone.
	RE::NiTransform ToShown(RE::TESObjectREFR* a_door)
	{
		auto* door3D = a_door->Get3D();
		auto* cell3D = door3D ? CellNodeAbove(a_door, door3D) : nullptr;
		return cell3D ? cell3D->world : RE::NiTransform{};
	}

	void Swing(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_room, float a_amount, bool a_otherEnd)
	{
		auto* door3D = a_door ? a_door->Get3D() : nullptr;
		if (!door3D || !a_door->GetBaseObject()) {
			return;
		}
		const auto id = a_door->GetFormID();
		std::scoped_lock lock(geometryLock);
		const auto&      geometry = GeometryOf(a_door);
		auto&            swung = swungClosed[id];
		if (!swung.empty() && (swung.size() != geometry.leaves.size() || swung.front().node != geometry.leaves.front().node)) {
			swung.clear();  // the door's 3D was reloaded: its new leaves are closed
		}
		if (swung.empty()) {
			for (const auto& leaf : geometry.leaves) {
				swung.push_back({ leaf.node, leaf.node->local });
			}
		}
		const auto closedWorld = ClosedWorld(a_door, door3D);
		const auto room = closedWorld.rotate.Transpose() * a_room;
		const bool wideX = !geometry.thinX;
		for (std::size_t i = 0; i < geometry.leaves.size() && i < swung.size(); ++i) {
			const auto& leaf = geometry.leaves[i];
			auto*       node = swung[i].node.get();
			// The hinge is the leaf's end its animation turns it around (its origin), or for a door
			// not animated, the low end; the menu's "other hinge" takes the other one.
			const float low = wideX ? leaf.box.min.x : leaf.box.min.y;
			const float high = wideX ? leaf.box.max.x : leaf.box.max.y;
			const float pivot = wideX ? leaf.pivot.x : leaf.pivot.y;
			bool        hingeHigh = node != door3D && std::abs(pivot - high) < std::abs(pivot - low);
			hingeHigh ^= a_otherEnd;
			const float        hingeEnd = hingeHigh ? high : low;
			const float        span = (high - low) * (hingeHigh ? -1.0f : 1.0f);
			const float        thinMiddle = wideX ? 0.5f * (leaf.box.min.y + leaf.box.max.y) : 0.5f * (leaf.box.min.x + leaf.box.max.x);
			const RE::NiPoint3 hinge = wideX ? RE::NiPoint3{ hingeEnd, thinMiddle, 0.0f } : RE::NiPoint3{ thinMiddle, hingeEnd, 0.0f };
			const RE::NiPoint3 free = wideX ? RE::NiPoint3{ span, 0.0f, 0.0f } : RE::NiPoint3{ 0.0f, span, 0.0f };
			const float        sign = (-free.y * room.x + free.x * room.y) >= 0.0f ? 1.0f : -1.0f;  // the free end turns towards the room
			const float        angle = sign * std::clamp(a_amount, 0.0f, 1.0f) * RE::deg_to_rad(95.0f);
			const float        c = std::cos(angle), s = std::sin(angle);
			RE::NiTransform    turn;  // in the door's model space
			turn.rotate = RE::NiMatrix3({ c, -s, 0.0f }, { s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
			turn.translate = hinge - turn.rotate * hinge;
			if (node == door3D) {
				node->local = swung[i].closedLocal * turn;
			} else {
				// The leaf's parent, in model space: the turn is carried into the leaf's own frame.
				const auto parent = node->parent ? ToRoot(id, node->parent, door3D) : RE::NiTransform{};
				node->local = parent.Invert() * turn * parent * swung[i].closedLocal;
			}
		}
		RE::NiUpdateData update;
		door3D->Update(update);
	}

	void Unswing(RE::FormID a_door)
	{
		std::scoped_lock lock(geometryLock);
		const auto       it = swungClosed.find(a_door);
		if (it == swungClosed.end()) {
			return;
		}
		for (auto& leaf : it->second) {
			leaf.node->local = leaf.closedLocal;
		}
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_door);
		auto* door3D = door ? door->Get3D() : nullptr;
		swungClosed.erase(it);
		if (door3D) {
			RE::NiUpdateData update;
			door3D->Update(update);
		}
	}

	void ForgetUnloaded()
	{
		std::scoped_lock lock(geometryLock);
		std::erase_if(geometries, [](const auto& a_entry) {
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_entry.first);
			return !door || door->Get3D() != a_entry.second.root.get();
		});
	}

	namespace
	{
		// A box in the door's model space, placed like the door (see Corners for the bit order).
		std::array<RE::NiPoint3, 8> CornersOf(RE::TESObjectREFR* a_door, const Box& a_box)
		{
			const auto                  world = Placement(a_door);
			std::array<RE::NiPoint3, 8> corners;
			for (int i = 0; i < 8; ++i) {
				const RE::NiPoint3 local{ i & 1 ? a_box.max.x : a_box.min.x, i & 2 ? a_box.max.y : a_box.min.y, i & 4 ? a_box.max.z : a_box.min.z };
				corners[i] = world * local;
			}
			return corners;
		}
	}

	std::array<RE::NiPoint3, 8> Corners(RE::TESObjectREFR* a_door)
	{
		Box box;
		{
			std::scoped_lock lock(geometryLock);
			box = GeometryOf(a_door).panel;
		}
		return CornersOf(a_door, box);
	}

	RE::NiPoint3 Center(RE::TESObjectREFR* a_door)
	{
		RE::NiPoint3 sum;
		for (const auto& c : Corners(a_door)) {
			sum += c;
		}
		return sum / 8.0f;
	}

	std::array<RE::NiPoint3, 4> Quad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, bool a_behindPanel, bool a_wholeMesh)
	{
		bool thinX;
		Box  box;
		{
			std::scoped_lock lock(geometryLock);
			const auto& geometry = GeometryOf(a_door);
			thinX = geometry.thinX;
			box = geometry.panel;
			if (a_wholeMesh) {
				// Across and up only: the faces stay the panel's.
				for (const auto& [shape, extent] : geometry.fixed) {
					(thinX ? box.min.y : box.min.x) = std::min(thinX ? box.min.y : box.min.x, thinX ? extent.min.y : extent.min.x);
					(thinX ? box.max.y : box.max.x) = std::max(thinX ? box.max.y : box.max.x, thinX ? extent.max.y : extent.max.x);
					box.min.z = std::min(box.min.z, extent.min.z);
					box.max.z = std::max(box.max.z, extent.max.z);
				}
			}
		}
		const auto c = CornersOf(a_door, box);
		const int  thinBit = thinX ? 1 : 2;
		const int  wideBit = thinX ? 2 : 1;
		RE::NiPoint3 outward = c[thinBit] - c[0];
		outward.Unitize();
		const bool maxSide = (a_viewer - (c[0] + c[7]) * 0.5f).Dot(outward) > 0.0f;
		// The panel's max face is the viewer's when maxSide; the quad goes 2 units off the chosen face,
		// away from the panel.
		const bool maxFace = maxSide != a_behindPanel;
		const int  face = maxFace ? thinBit : 0;
		const auto shift = outward * (maxFace ? 2.0f : -2.0f);
		return { c[face] + shift, c[face | wideBit] + shift, c[face | 4] + shift, c[face | wideBit | 4] + shift };
	}

	std::array<RE::NiPoint3, 4> ShownQuad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, bool a_behindPanel, bool a_wholeMesh)
	{
		const auto toShown = ToShown(a_door);
		auto       quad = Quad(a_door, toShown.Invert() * a_viewer, a_behindPanel, a_wholeMesh);
		for (auto& corner : quad) {
			corner = toShown * corner;
		}
		return quad;
	}

	RE::NiPoint3 ShownCenter(RE::TESObjectREFR* a_door)
	{
		return ToShown(a_door) * Center(a_door);
	}

	PanelSize Size(RE::TESObjectREFR* a_door)
	{
		Box   box;
		bool  thinX;
		{
			std::scoped_lock lock(geometryLock);
			const auto& geometry = GeometryOf(a_door);
			box = geometry.panel;
			thinX = geometry.thinX;
		}
		const float scale = a_door->Get3D()->world.scale;
		const float x = box.max.x - box.min.x, y = box.max.y - box.min.y;
		return { 0.5f * scale * (thinX ? y : x), 0.5f * scale * (thinX ? x : y), 0.5f * scale * (box.max.z - box.min.z) };
	}

	namespace
	{
		// The shapes standing behind the panel, away from the door's own side (where its partner's
		// marker puts whoever comes through): the box that hides the void behind a closed door.
		std::vector<RE::NiPointer<RE::BSGeometry>> BehindPanel(RE::TESObjectREFR* a_door)
		{
			auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
			auto  partner = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
			auto* partnerTeleport = partner ? partner->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
			if (!partnerTeleport || !partnerTeleport->teleportData) {
				return {};
			}
			const auto  front = Placement(a_door).Invert() * partnerTeleport->teleportData->position;
			std::scoped_lock lock(geometryLock);
			const auto& geometry = GeometryOf(a_door);
			if (!geometry.measured) {
				return {};
			}
			const auto  along = [&](const RE::NiPoint3& a_point) { return geometry.thinX ? a_point.x : a_point.y; };
			const float low = along(geometry.panel.min), high = along(geometry.panel.max);
			const bool  frontHigh = along(front) > 0.5f * (low + high);
			std::vector<RE::NiPointer<RE::BSGeometry>> behind;
			for (const auto& [shape, box] : geometry.fixed) {
				// Reaching out behind the panel and nowhere in front of its face (the dragon door's box
				// starts inside the panel's thickness); a frame reaches out on both sides.
				const bool isBehind = frontHigh ? along(box.max) <= high + 1.0f && along(box.min) < low : along(box.min) >= low - 1.0f && along(box.max) > high;
				if (isBehind) {
					behind.push_back(shape);
				}
			}
			return behind;
		}
	}

	namespace
	{
		std::mutex                            correctionsLock;
		std::unordered_map<RE::FormID, float> corrections;  // under correctionsLock
		std::unordered_map<RE::FormID, int>   styles;       // under correctionsLock
		std::unordered_set<RE::FormID>        portalsOff;   // under correctionsLock, keyed by CorrectionDoor
		std::unordered_set<RE::FormID>        opensInward;  // under correctionsLock, keyed by CorrectionDoor
		std::unordered_set<RE::FormID>        alwaysOpen;   // under correctionsLock, keyed by CorrectionDoor
		std::unordered_map<RE::FormID, RE::NiPoint3> shifts;  // under correctionsLock
		bool                                  correctionsLoaded = false;

		std::filesystem::path CorrectionsFile()
		{
			return Paths::PluginDirectory() / "SeamlessSkyrim_Doors.ini";
		}

		// "Skyrim.esm|01A6F5": the same door whatever the load order.
		std::string CorrectionKey(RE::FormID a_door)
		{
			auto* form = RE::TESForm::LookupByID(a_door);
			auto* file = form ? form->GetFile(0) : nullptr;
			if (!file) {
				return std::format("{:08X}", a_door);
			}
			const auto local = file->IsLight() ? (a_door & 0xFFF) : (a_door & 0xFFFFFF);
			return std::format("{}|{:06X}", file->GetFilename(), local);
		}

		void LoadCorrections()
		{
			if (std::exchange(correctionsLoaded, true)) {
				return;
			}
			std::wstring buffer(32768, L'\0');
			const auto   length = ::GetPrivateProfileSectionW(L"DoorTurn", buffer.data(), static_cast<DWORD>(buffer.size()), CorrectionsFile().c_str());
			auto*        handler = RE::TESDataHandler::GetSingleton();
			for (std::size_t at = 0; at < length && buffer[at];) {
				const std::wstring entry(buffer.data() + at);
				at += entry.size() + 1;
				const auto equals = entry.find(L'=');
				const auto bar = entry.find(L'|');
				if (equals == std::wstring::npos || bar == std::wstring::npos || bar > equals || !handler) {
					continue;
				}
				const std::string file = Paths::Narrow(std::wstring_view(entry).substr(0, bar));
				const auto        local = static_cast<RE::FormID>(std::wcstoul(entry.substr(bar + 1, equals - bar - 1).c_str(), nullptr, 16));
				const float       degrees = std::wcstof(entry.c_str() + equals + 1, nullptr);
				if (const auto id = handler->LookupFormID(local, file); id && degrees != 0.0f) {
					corrections[id] = degrees;
				}
			}
			// [DoorHinge] Plugin.esp|localID=style (OpenStyle). The older [DoorOpen] and [DoorShift]
			// were made against doors measured by their bounds: the hinge guessed from them, the
			// shift mostly making up for the bounds' depth. Against the measured panel neither
			// means the same, so they are left unread.
			std::fill(buffer.begin(), buffer.end(), L'\0');
			const auto styleLength = ::GetPrivateProfileSectionW(L"DoorHinge", buffer.data(), static_cast<DWORD>(buffer.size()), CorrectionsFile().c_str());
			for (std::size_t at = 0; at < styleLength && buffer[at];) {
				const std::wstring entry(buffer.data() + at);
				at += entry.size() + 1;
				const auto equals = entry.find(L'=');
				const auto bar = entry.find(L'|');
				if (equals == std::wstring::npos || bar == std::wstring::npos || bar > equals || !handler) {
					continue;
				}
				const std::string file = Paths::Narrow(std::wstring_view(entry).substr(0, bar));
				const auto        local = static_cast<RE::FormID>(std::wcstoul(entry.substr(bar + 1, equals - bar - 1).c_str(), nullptr, 16));
				if (const auto id = handler->LookupFormID(local, file)) {
					styles[id] = std::wcstol(entry.c_str() + equals + 1, nullptr, 10);
				}
			}
			// [DoorPortal] Plugin.esp|localID=0: the pair's portal switched off.
			std::fill(buffer.begin(), buffer.end(), L'\0');
			const auto portalLength = ::GetPrivateProfileSectionW(L"DoorPortal", buffer.data(), static_cast<DWORD>(buffer.size()), CorrectionsFile().c_str());
			for (std::size_t at = 0; at < portalLength && buffer[at];) {
				const std::wstring entry(buffer.data() + at);
				at += entry.size() + 1;
				const auto equals = entry.find(L'=');
				const auto bar = entry.find(L'|');
				if (equals == std::wstring::npos || bar == std::wstring::npos || bar > equals || !handler) {
					continue;
				}
				const std::string file = Paths::Narrow(std::wstring_view(entry).substr(0, bar));
				const auto        local = static_cast<RE::FormID>(std::wcstoul(entry.substr(bar + 1, equals - bar - 1).c_str(), nullptr, 16));
				if (const auto id = handler->LookupFormID(local, file); id && std::wcstol(entry.c_str() + equals + 1, nullptr, 10) == 0) {
					portalsOff.insert(id);
				}
			}
			// [DoorOpensIn] and [DoorAlwaysOpen] Plugin.esp|localID=1.
			for (const auto& [section, set] : { std::pair{ L"DoorOpensIn", &opensInward }, std::pair{ L"DoorAlwaysOpen", &alwaysOpen } }) {
				std::fill(buffer.begin(), buffer.end(), L'\0');
				const auto length = ::GetPrivateProfileSectionW(section, buffer.data(), static_cast<DWORD>(buffer.size()), CorrectionsFile().c_str());
				for (std::size_t at = 0; at < length && buffer[at];) {
					const std::wstring entry(buffer.data() + at);
					at += entry.size() + 1;
					const auto equals = entry.find(L'=');
					const auto bar = entry.find(L'|');
					if (equals == std::wstring::npos || bar == std::wstring::npos || bar > equals || !handler) {
						continue;
					}
					const std::string file = Paths::Narrow(std::wstring_view(entry).substr(0, bar));
					const auto        local = static_cast<RE::FormID>(std::wcstoul(entry.substr(bar + 1, equals - bar - 1).c_str(), nullptr, 16));
					if (const auto id = handler->LookupFormID(local, file); id && std::wcstol(entry.c_str() + equals + 1, nullptr, 10) != 0) {
						set->insert(id);
					}
				}
			}
			// [DoorOffset] Plugin.esp|localID=x,y,z
			std::fill(buffer.begin(), buffer.end(), L'\0');
			const auto shiftLength = ::GetPrivateProfileSectionW(L"DoorOffset", buffer.data(), static_cast<DWORD>(buffer.size()), CorrectionsFile().c_str());
			for (std::size_t at = 0; at < shiftLength && buffer[at];) {
				const std::wstring entry(buffer.data() + at);
				at += entry.size() + 1;
				const auto equals = entry.find(L'=');
				const auto bar = entry.find(L'|');
				if (equals == std::wstring::npos || bar == std::wstring::npos || bar > equals || !handler) {
					continue;
				}
				const std::string file = Paths::Narrow(std::wstring_view(entry).substr(0, bar));
				const auto        local = static_cast<RE::FormID>(std::wcstoul(entry.substr(bar + 1, equals - bar - 1).c_str(), nullptr, 16));
				RE::NiPoint3      shift;
				if (std::swscanf(entry.c_str() + equals + 1, L"%f,%f,%f", &shift.x, &shift.y, &shift.z) == 3) {
					if (const auto id = handler->LookupFormID(local, file)) {
						shifts[id] = shift;
					}
				}
			}
			logger::info("portal: {} door turn(s), {} door shift(s), {} door style(s), {} portal(s) off, {} opening inward, {} always open loaded", corrections.size(), shifts.size(),
				styles.size(), portalsOff.size(), opensInward.size(), alwaysOpen.size());
		}

		RE::NiTransform TurnAround(float a_radians, const RE::NiPoint3& a_center)
		{
			const float     c = std::cos(a_radians), s = std::sin(a_radians);
			RE::NiTransform turn;
			turn.rotate = RE::NiMatrix3({ c, -s, 0.0f }, { s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
			turn.translate = a_center - turn.rotate * a_center;
			return turn;
		}
	}

	RE::TESObjectREFR* CorrectionDoor(RE::TESObjectREFR* a_source, RE::TESObjectREFR* a_linked)
	{
		// A persistent street door has no parent cell while the player is indoors (a save loaded
		// there): told by its worldspace, as the manager's Outdoors does. Taken for indoors, the
		// menu stored a door's settings under the pair's other door, where the swing never read them.
		const auto outdoors = [](RE::TESObjectREFR* a_door) {
			auto* cell = a_door->GetParentCell();
			return cell ? !cell->IsInteriorCell() : a_door->GetWorldspace() != nullptr;
		};
		const bool sourceOut = outdoors(a_source);
		const bool linkedOut = outdoors(a_linked);
		return CorrectionDoor(a_source->GetFormID(), sourceOut, a_linked->GetFormID(), linkedOut) == a_source->GetFormID() ? a_source : a_linked;
	}

	RE::FormID CorrectionDoor(RE::FormID a_source, bool a_sourceOutdoors, RE::FormID a_linked, bool a_linkedOutdoors)
	{
		if (a_sourceOutdoors != a_linkedOutdoors) {
			return a_sourceOutdoors ? a_source : a_linked;
		}
		return a_source < a_linked ? a_source : a_linked;
	}

	RE::FormID SwingingDoor(RE::FormID a_source, bool a_sourceOutdoors, RE::FormID a_linked, bool a_linkedOutdoors)
	{
		const auto key = CorrectionDoor(a_source, a_sourceOutdoors, a_linked, a_linkedOutdoors);
		return OpensInward(key) == (key == a_source) ? a_linked : a_source;
	}

	float Correction(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = corrections.find(a_door);
		return it != corrections.end() ? it->second : 0.0f;
	}

	void SetCorrection(RE::FormID a_door, float a_degrees)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		corrections[a_door] = a_degrees;
	}

	void SaveCorrection(RE::FormID a_door)
	{
		const float degrees = Correction(a_door);
		const int   style = static_cast<int>(Style(a_door));
		const auto  key = CorrectionKey(a_door);
		const std::wstring wkey = Paths::Widen(key);
		const auto         value = std::format(L"{:.2f}", degrees);
		const auto         styleValue = std::format(L"{}", style);
		::WritePrivateProfileStringW(L"DoorTurn", wkey.c_str(), degrees != 0.0f ? value.c_str() : nullptr, CorrectionsFile().c_str());
		::WritePrivateProfileStringW(L"DoorHinge", wkey.c_str(), style != 0 ? styleValue.c_str() : nullptr, CorrectionsFile().c_str());
		::WritePrivateProfileStringW(L"DoorOpen", wkey.c_str(), nullptr, CorrectionsFile().c_str());
		::WritePrivateProfileStringW(L"DoorShift", wkey.c_str(), nullptr, CorrectionsFile().c_str());
		const auto shift = Shift(a_door);
		const auto shiftValue = std::format(L"{:.1f},{:.1f},{:.1f}", shift.x, shift.y, shift.z);
		const bool shifted = shift.x != 0.0f || shift.y != 0.0f || shift.z != 0.0f;
		::WritePrivateProfileStringW(L"DoorOffset", wkey.c_str(), shifted ? shiftValue.c_str() : nullptr, CorrectionsFile().c_str());
		const bool off = PortalOff(a_door);
		const bool inward = OpensInward(a_door);
		const bool open = AlwaysOpen(a_door);
		::WritePrivateProfileStringW(L"DoorPortal", wkey.c_str(), off ? L"0" : nullptr, CorrectionsFile().c_str());
		::WritePrivateProfileStringW(L"DoorOpensIn", wkey.c_str(), inward ? L"1" : nullptr, CorrectionsFile().c_str());
		::WritePrivateProfileStringW(L"DoorAlwaysOpen", wkey.c_str(), open ? L"1" : nullptr, CorrectionsFile().c_str());
		logger::info("portal: door {} saved: turned {:.2f} degrees, opens with style {}{}{}{}", key, degrees, style,
			off ? ", portal off" : ", portal on", inward ? ", opens inward" : "", open ? ", always open" : "");
	}

	RE::NiPoint3 Shift(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = shifts.find(a_door);
		return it != shifts.end() ? it->second : RE::NiPoint3{};
	}

	void SetShift(RE::FormID a_door, const RE::NiPoint3& a_shift)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		shifts[a_door] = a_shift;
	}

	bool PortalOff(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return portalsOff.contains(a_door);
	}

	void SetPortalOff(RE::FormID a_door, bool a_off)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		if (a_off) {
			portalsOff.insert(a_door);
		} else {
			portalsOff.erase(a_door);
		}
	}

	bool OpensInward(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return opensInward.contains(a_door);
	}

	void SetOpensInward(RE::FormID a_door, bool a_inward)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		if (a_inward) {
			opensInward.insert(a_door);
		} else {
			opensInward.erase(a_door);
		}
	}

	bool AlwaysOpen(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return alwaysOpen.contains(a_door);
	}

	void SetAlwaysOpen(RE::FormID a_door, bool a_open)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		if (a_open) {
			alwaysOpen.insert(a_door);
		} else {
			alwaysOpen.erase(a_door);
		}
	}

	std::vector<RE::FormID> AlwaysOpenDoors()
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return { alwaysOpen.begin(), alwaysOpen.end() };
	}

	OpenStyle Style(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = styles.find(a_door);
		return it != styles.end() && it->second == static_cast<int>(OpenStyle::kSwingOtherEnd) ? OpenStyle::kSwingOtherEnd : OpenStyle::kSwing;
	}

	void SetStyle(RE::FormID a_door, OpenStyle a_style)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		styles[a_door] = static_cast<int>(a_style);
	}

	Mapping Compute(const Manager::Active& a_portal)
	{
		// Orientations come from the doors' teleport markers, not from the door meshes: meshes are
		// not placed consistently, the markers say how the game itself turns you. The source door's
		// marker is where you appear inside and which way you face; the linked door's marker is
		// where you appear outside, facing away from the door.
		const auto yawOf = [](RE::TESObjectREFR* a_door) {
			auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
			if (!teleport || !teleport->teleportData) {
				throw std::runtime_error(std::format("door {:08X} has no teleport marker", a_door->GetFormID()));
			}
			return teleport->teleportData->rotation.z;
		};
		// A frame whose local +y is the heading `a_yaw` (Skyrim: forward = (sin, cos)), +z up.
		const auto frame = [](float a_yaw, const RE::NiPoint3& a_origin) {
			const float     s = std::sin(a_yaw), c = std::cos(a_yaw);
			RE::NiTransform t;
			t.rotate = RE::NiMatrix3({ c, s, 0.0f }, { -s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
			t.translate = a_origin;
			return t;
		};
		auto*       source = Door(a_portal.sourceDoor);
		auto*       linked = Door(a_portal.linkedDoor);
		// Outside: walking into the source door = facing opposite to how you come out of it.
		const float inYaw = yawOf(linked) + RE::NI_PI;
		const float outYaw = yawOf(source);
		const auto  outside = frame(inYaw, Center(source));
		const auto  inside = frame(outYaw, Center(linked));

		Mapping mapping;
		mapping.toLinked = inside * outside.Invert();
		mapping.sourceCenter = Center(source);
		mapping.inward = { std::sin(inYaw), std::cos(inYaw), 0.0f };
		mapping.yawDelta = outYaw - inYaw;
		// The hand correction turns the inside around its own door: seen from the door that holds
		// it, the other side's picture turns; from the other side, the way back turns the opposite way.
		auto* key = CorrectionDoor(source, linked);
		if (const float degrees = Correction(key->GetFormID()); degrees != 0.0f) {
			const float turn = RE::deg_to_rad(degrees);
			if (key == source) {
				mapping.toLinked = TurnAround(turn, Center(linked)) * mapping.toLinked;
				mapping.yawDelta += turn;
			} else {
				mapping.toLinked = mapping.toLinked * TurnAround(-turn, Center(source));
				mapping.yawDelta -= turn;
			}
		}
		// The shift moves the inside's picture in the key door's space (from there, toLinked reads
		// a point d further back; the way back adds d again).
		if (const auto shift = Shift(key->GetFormID()); shift.x != 0.0f || shift.y != 0.0f || shift.z != 0.0f) {
			const float  yaw = yawOf(key == source ? linked : source) + RE::NI_PI;  // a door's marker stands at the other door; walking in faces against it
			const RE::NiPoint3 in{ std::sin(yaw), std::cos(yaw), 0.0f };
			const RE::NiPoint3 right{ in.y, -in.x, 0.0f };
			RE::NiTransform move;
			move.translate = right * shift.x + in * shift.y + RE::NiPoint3{ 0.0f, 0.0f, shift.z };
			if (key == source) {
				mapping.toLinked = mapping.toLinked * move.Invert();
			} else {
				mapping.toLinked = move * mapping.toLinked;
			}
		}
		const auto size = Size(source);
		mapping.halfWidth = size.halfWidth;
		mapping.halfDepth = size.halfDepth;
		mapping.halfHeight = size.halfHeight;
		return mapping;
	}

	namespace
	{
		// One door can be held open twice at once: as the portal door of the space the player is in
		// and as the door they just came out of (Travel). Its original collision is recorded by the
		// first opener and restored when the last one lets go.
		struct Shared
		{
			int                                                                  opens = 0;
			int                                                                  hides = 0;
			int                                                                  passables = 0;
			int                                                                  walkables = 0;  // openers not solid (OpenState::solid)
			std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, RE::COL_LAYER>> layers;
			std::vector<RE::NiPointer<RE::BSGeometry>>                           hiddenShapes;
			std::vector<RE::NiPointer<RE::BSGeometry>>                           behindShapes;
			// Taken out of the Havok world while the door is passable (HideBodies), and the 3D they
			// belong to: put back only into that.
			std::vector<RE::NiPointer<RE::bhkWorldObject>>                       removedBodies;
			RE::NiPointer<RE::NiAVObject>                                        bodiesOf;
		};

		// The layer change alone didn't stop a city gate: Whiterun's gate kept the player 25 units
		// short of its doorway (in first person often just short enough for the crossing, which needs
		// the camera at the picture, not to happen), and it stopped rays as well.
		void HideBodies(RE::NiAVObject* a_door3D, Shared& a_shared)
		{
			a_shared.bodiesOf.reset(a_door3D);
			RE::BSVisit::TraverseScenegraphObjects(a_door3D, [&](RE::NiAVObject* a_object) {
				auto* collision = a_object->collisionObject ? a_object->collisionObject->AsBhkNiCollisionObject() : nullptr;
				if (collision && collision->body && collision->body->GetWorld1()) {
					a_shared.removedBodies.emplace_back(collision->body);
					collision->body->RemoveFromCurrentWorld();
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		void ShowBodies(RE::TESObjectREFR* a_door, Shared& a_shared)
		{
			auto* cell = a_door ? a_door->GetParentCell() : nullptr;
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			// Only into the 3D they came from, still standing in an attached cell: a cell let go in the
			// meantime has taken its world apart, and a reloaded door has bodies of its own.
			if (world && a_door->Get3D() == a_shared.bodiesOf.get() && cell->cellState.get() == RE::TESObjectCELL::CellState::kAttached) {
				for (auto& body : a_shared.removedBodies) {
					if (!body->GetWorld1()) {
						body->MoveToWorld(world);
					}
				}
			}
			a_shared.removedBodies.clear();
			a_shared.bodiesOf.reset();
		}
		std::unordered_map<RE::FormID, Shared> openDoors;

		constexpr auto kOpenDoorFlags = static_cast<RE::ExtraFlags::Flag>(
			static_cast<std::uint32_t>(RE::ExtraFlags::Flag::kBlockPlayerActivate) | static_cast<std::uint32_t>(RE::ExtraFlags::Flag::kBlockActivateText));
	}

	bool IsOpen(RE::FormID a_door)
	{
		return openDoors.contains(a_door);
	}

	// Held open walkable: its prompt and ordinary transition are off (Open).
	bool PromptOff(RE::FormID a_door)
	{
		const auto it = openDoors.find(a_door);
		return it != openDoors.end() && it->second.walkables > 0;
	}

	bool BodiesHidden(RE::FormID a_door)
	{
		const auto it = openDoors.find(a_door);
		return it != openDoors.end() && !it->second.removedBodies.empty();
	}

	// TESObjectDOOR::GetActivateText (vfunc 0x4C) builds the "Open <destination>" line for the door
	// under the crosshair. The extra flags that block activation text didn't keep it off the
	// screen for an open doorway; for a door held open by a portal the text is left empty here.
	struct ActivateTextHook
	{
		static inline std::uint32_t calls = 0;
		static bool Thunk(RE::TESObjectDOOR* a_door, RE::TESObjectREFR* a_activator, RE::BSString& a_text)
		{
			++calls;
			auto* pick = RE::CrosshairPickData::GetSingleton();
			const auto target = pick ? pick->GetActiveTarget().get() : nullptr;
			if (target && PromptOff(target->GetFormID())) {
				a_text = "";
				return false;
			}
			return func(a_door, a_activator, a_text);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The text hook above alone left the prompt on the HUD (measured: "Open Riverwood Trader"
	// with the crosshair on a hidden, open door), so the HUD is also told, the way the engine
	// tells it when the crosshair leaves an object.
	std::uint32_t HidePrompt()
	{
		auto*      pick = RE::CrosshairPickData::GetSingleton();
		const auto target = pick ? pick->GetActiveTarget().get() : nullptr;
		if (target && PromptOff(target->GetFormID())) {
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
			if (menu && menu->uiMovie) {
				RE::GFxValue text;
				if (menu->uiMovie->GetVariable(&text, "_root.HUDMovieBaseInstance.RolloverText.text") && text.IsString() && text.GetString()[0] != 0) {
					const RE::GFxValue args[2]{ false, "" };
					menu->uiMovie->Invoke("_root.HUDMovieBaseInstance.SetCrosshairTarget", nullptr, args, 2);
				}
			}
		}
		return ActivateTextHook::calls;
	}

	void InstallPromptHook()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_TESObjectDOOR[0] };
		ActivateTextHook::func = vtable.write_vfunc(0x4C, ActivateTextHook::Thunk);
	}

	void Open(RE::TESObjectREFR* a_door, OpenState& a_state, bool a_hide, bool a_passable, bool a_solid)
	{
		auto* door3D = a_door ? a_door->Get3D() : nullptr;
		if (!door3D || (a_state.door == a_door->GetFormID() && a_state.hidden == a_hide && a_state.passable == a_passable && a_state.solid == a_solid)) {
			return;
		}
		Close(a_state);
		a_state.door = a_door->GetFormID();
		a_state.solid = a_solid;
		auto& shared = openDoors[a_state.door];
		if (shared.opens++ == 0) {
			// What stands behind the panel only to hide the void there would now block the view
			// through the doorway; it went round with the panel when that was swung as a whole.
			for (auto& shape : BehindPanel(a_door)) {
				if (!shape->GetAppCulled()) {
					shape->SetAppCulled(true);
					shared.behindShapes.push_back(shape);
				}
			}
		}
		if (!a_solid && shared.walkables++ == 0) {
			// The doorway can be walked through: no "E) Open ..." prompt on the panel that isn't
			// there any more, and no ordinary door transition from it (it would end in a loading
			// screen). A door only shown open (a stand-in's, solid) keeps both: its space isn't
			// there to walk into, and blocked it was a door that couldn't be used at all.
			a_door->extraList.SetExtraFlags(kOpenDoorFlags, true);
			RE::BSVisit::TraverseScenegraphObjects(door3D, [&](RE::NiAVObject* a_object) {
				if (a_object->collisionObject) {
					const auto layer = a_object->GetCollisionLayer();
					if (layer != RE::COL_LAYER::kUnidentified && layer != RE::COL_LAYER::kNonCollidable) {
						shared.layers.emplace_back(RE::NiPointer<RE::NiAVObject>(a_object), layer);
						a_object->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);
					} else {
						logger::info("portal: door {:08X}: collision on '{}' left as it is (layer {})", a_door->GetFormID(), a_object->name.c_str(), static_cast<int>(layer));
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
		if (a_hide) {
			if (shared.hides++ == 0) {
				// Culling the root alone left the panel on screen while merged; the shapes are
				// culled one by one, as for the player's body (CameraLimit).
				door3D->SetAppCulled(true);
				RE::BSVisit::TraverseScenegraphGeometries(door3D, [&](RE::BSGeometry* a_shape) {
					if (!a_shape->GetAppCulled()) {
						a_shape->SetAppCulled(true);
						shared.hiddenShapes.emplace_back(a_shape);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			a_state.hidden = true;
		}
		if (a_passable) {
			if (shared.passables++ == 0) {
				HideBodies(door3D, shared);
			}
			a_state.passable = true;
		}
		logger::info("portal: door {:08X} open ({} collision objects, {} shape(s) behind the panel off, {} opener(s){}{})", a_state.door, shared.layers.size(), shared.behindShapes.size(), shared.opens, a_hide ? ", hidden" : "",
			a_passable ? std::format(", passable: {} bodies out", shared.removedBodies.size()) : "");
	}

	void SetOpenFlags(bool a_set)
	{
		for (const auto& [id, shared] : openDoors) {
			if (shared.walkables == 0) {
				continue;
			}
			if (auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id)) {
				door->extraList.SetExtraFlags(kOpenDoorFlags, a_set);
			}
		}
	}

	void Close(OpenState& a_state)
	{
		const auto it = a_state.door ? openDoors.find(a_state.door) : openDoors.end();
		if (it != openDoors.end()) {
			auto& shared = it->second;
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_state.door);
			auto* door3D = door ? door->Get3D() : nullptr;
			if (a_state.hidden && --shared.hides == 0) {
				if (door3D) {
					door3D->SetAppCulled(false);
				}
				for (auto& shape : shared.hiddenShapes) {
					shape->SetAppCulled(false);
				}
				shared.hiddenShapes.clear();
			}
			if (a_state.passable && --shared.passables == 0) {
				ShowBodies(door, shared);
			}
			if (!a_state.solid && --shared.walkables == 0) {
				if (door) {
					door->extraList.SetExtraFlags(kOpenDoorFlags, false);
				}
				for (auto& [object, layer] : shared.layers) {
					object->SetCollisionLayer(layer);
				}
				shared.layers.clear();
			}
			if (--shared.opens == 0) {
				for (auto& shape : shared.behindShapes) {
					shape->SetAppCulled(false);
				}
				openDoors.erase(it);
			}
		}
		a_state = {};
	}
}