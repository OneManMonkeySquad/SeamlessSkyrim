#include "Portal/LightOcclusion.h"

#include "Portal/PortalManager.h"

namespace
{
	struct Key
	{
		const RE::BSGeometry* geometry;
		const RE::NiLight*    light;
		bool operator==(const Key&) const = default;
	};

	struct KeyHash
	{
		std::size_t operator()(const Key& a_key) const noexcept
		{
			return std::hash<const void*>()(a_key.geometry) ^ (std::hash<const void*>()(a_key.light) << 1);
		}
	};

	struct Entry
	{
		float        visibility;
		RE::NiPoint3 center;  // where the object was when measured (actors move)
	};

	// See OutsideShell; dropped with cache. With where the light was: a light carried about (a
	// torch) or a new light at a freed one's address is judged again.
	struct ShellVerdict
	{
		bool         outside = false;
		RE::NiPoint3 at;
	};
	// One per room merged at once (Render::Backend::MergedRooms), by the room's slot: each room's
	// lights are measured in its own Havok world, placed by its own door. Slot 0 also serves the
	// portal pass.
	struct Space
	{
		RE::bhkWorld*                                        world = nullptr;
		std::uint32_t                                        generation = UINT32_MAX;
		RE::NiTransform                                      toPhysics;  // see SetWorld
		std::optional<Portal::LightOcclusion::RoomDoor>      door;       // see SetWorld
		std::unordered_map<Key, Entry, KeyHash>              cache;
		std::unordered_map<const RE::NiLight*, ShellVerdict> outsideShell;
		bool                                                 interior = false;  // see SetWorld
	};
	std::array<Space, 4> spaces;
	std::uint32_t                              raysThisFrame = 0;
	std::uint32_t                              unmeasuredThisFrame = 0;
	std::uint64_t                              raysTotal = 0;
	// Measuring a whole room at once (thousands of pairs) would be one long frame; the rest is
	// measured over the next frames, and until then a pair counts as half lit.
	constexpr std::uint32_t kRayBudgetPerFrame = 4000;
	constexpr float         kUnmeasured = 0.5f;
	// The door panel's half thickness and a little: a point on the panel is still in the room.
	constexpr float kDoorSlack = 8.0f;
	// Around the opening: the frame, which stands where the closed door's shadow would fall.
	constexpr float kDoorMargin = 64.0f;

	// In the room's own world its door is closed, and the room's light stops there. Its Havok
	// world has the door open (the portal holds it open, for walking through), so rays pass:
	// Lux's strong lights outside the entrance lit the floor inside it, and the frame's outer step
	// (which sticks out beyond the door, where nothing of the room shields it) glowed from the
	// street.
	bool ThroughClosedDoor(const Space& a_space, const RE::NiPoint3& a_light, const RE::NiPoint3& a_point)
	{
		const auto& door = a_space.door;
		if (!door) {
			return false;
		}
		const auto right = RE::NiPoint3{ 0.0f, 0.0f, 1.0f }.Cross(door->intoRoom);
		const auto inOpening = [&](const RE::NiPoint3& a_at) {
			const auto offset = a_at - door->center;
			return std::abs(offset.Dot(right)) < door->halfWidth + kDoorMargin && std::abs(offset.z) < door->halfHeight + kDoorMargin;
		};
		const float lightSide = (a_light - door->center).Dot(door->intoRoom);
		const float pointSide = (a_point - door->center).Dot(door->intoRoom);
		// Just outside the door, in front of the opening (the frame's outer step): the closed
		// door's shadow from the room, or the street the room doesn't light. Only just: unbounded,
		// a whole column behind the entrance's plane went dark (a room reaching round its entry).
		if (pointSide < -kDoorSlack && pointSide > -(kDoorSlack + kDoorMargin) && inOpening(a_point)) {
			return true;
		}
		// Otherwise only a line crossing the door's plane passes the closed door.
		if ((lightSide >= -kDoorSlack) == (pointSide >= -kDoorSlack)) {
			return false;
		}
		return inOpening(a_light + (a_point - a_light) * (lightSide / (lightSide - pointSide)));
	}

	// Whether the straight line between the two points is free of static collision (line-of-sight
	// layer, like the engine's own LOS checks). A hit in the last few units counts as free: that
	// is the surface the point lies on.
	bool Clear(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		const float     scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pick;
		pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
		pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
		++raysThisFrame;
		++raysTotal;
		if (!a_world->PickObject(pick) || !pick.rayOutput.HasHit()) {
			return true;
		}
		const float length = a_from.GetDistance(a_to);
		return (1.0f - pick.rayOutput.hitFraction) * length < 12.0f;
	}

	// How far a ray from a light goes before it counts as gone into the void around the cell. Four
	// cells: at one, the far walls of a big hall (a Dwemer one, a cavern) counted as void, and
	// lights in the middle of it were left out.
	constexpr float kShellReach = 16384.0f;

	// Whether a light stands outside the room's shell: above its roof or behind a window, where
	// Lux puts strong lights whose shadows draw sunbeams through the openings. An interior has no
	// world around it, so most rays from such a light leave into the void; from a light in the
	// room they meet a wall, the floor or the ceiling (a ceiling without collision lets one out).
	// Measured per object, such a light lit whole floor pieces that a grate's opening saw:
	// Whiterun's Hall of the Dead, seen from the street, had squares of sunlight on its floor.
	bool OutsideShell(RE::bhkWorld* a_world, const RE::NiPoint3& a_source)
	{
		const RE::NiPoint3 directions[] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		const float        scale = RE::bhkWorld::GetWorldScale();
		int                escaped = 0;
		for (const auto& direction : directions) {
			const auto      to = a_source + direction * kShellReach;
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_source.x * scale, a_source.y * scale, a_source.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0.0f);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
			++raysThisFrame;
			++raysTotal;
			if (!a_world->PickObject(pick) || !pick.rayOutput.HasHit()) {
				++escaped;
			}
		}
		return escaped >= 3;
	}

	// Its beams need the shadow map this view doesn't have: left out, rather than lighting whole
	// shapes that only a sliver of the beam falls on. Six rays once per light, so judged outside
	// the ray budget: within it, the shapes not measured yet were half lit by it meanwhile, and a
	// view of the whole room at once (the doorway picture's camera) had Whiterun's Hall of the
	// Dead and Jorrvaskr's Warmaidens far too bright.
	bool LightOutside(Space& a_space, const RE::NiLight* a_light)
	{
		const auto source = a_space.toPhysics * a_light->world.translate;
		auto [verdict, added] = a_space.outsideShell.try_emplace(a_light);
		if (added || verdict->second.at.GetSquaredDistance(source) > 25.0f * 25.0f) {
			verdict->second = { a_space.interior && OutsideShell(a_space.world, source), source };
		}
		return verdict->second.outside;
	}

	float Measure(const Space& a_space, const RE::BSGeometry* a_geometry, const RE::NiLight* a_light)
	{
		const auto& bound = a_geometry->worldBound;
		const auto  source = a_space.toPhysics * a_light->world.translate;
		// The bound's centre and six points around it: big shapes (a floor, a wall) are partly lit.
		const float  r = bound.radius * 0.6f;
		const RE::NiPoint3 offsets[] = { {}, { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, r * 0.5f }, { 0, 0, -r * 0.5f } };
		int          seen = 0;
		for (const auto& offset : offsets) {
			const auto point = a_space.toPhysics * (bound.center + offset);
			seen += !ThroughClosedDoor(a_space, source, point) && Clear(a_space.world, source, point) ? 1 : 0;
		}
		return static_cast<float>(seen) / std::size(offsets);
	}
}

namespace Portal::LightOcclusion
{
	void SetWorld(int a_slot, RE::bhkWorld* a_world, std::uint32_t a_generation, const RE::NiTransform& a_toPhysics, const std::optional<RoomDoor>& a_door, bool a_interior)
	{
		if (a_slot < 0 || a_slot >= static_cast<int>(spaces.size())) {
			return;
		}
		auto& [world, generation, toPhysics, door, cache, outsideShell, interior] = spaces[a_slot];
		const bool doorMoved = a_door.has_value() != door.has_value() ||
		                       (a_door && (a_door->center.GetDistance(door->center) > 0.01f || a_door->intoRoom.GetDistance(door->intoRoom) > 0.001f));
		// Cached per drawn position, so a room placed anew (merged, unmerged, a door correction)
		// is measured anew.
		const bool moved = std::memcmp(&a_toPhysics.rotate, &toPhysics.rotate, sizeof(RE::NiMatrix3)) != 0 ||
		                   a_toPhysics.translate.GetDistance(toPhysics.translate) > 0.01f;
		if (a_generation != generation || !a_world || moved || doorMoved || a_interior != interior) {
			cache.clear();
			outsideShell.clear();
		}
		door = a_door;
		interior = a_interior;
		world = a_world;
		generation = a_generation;
		toPhysics = a_toPhysics;
		if (a_slot == 0) {
			// Once per frame: slot 0 is set every frame, before the others.
			raysThisFrame = 0;
			unmeasuredThisFrame = 0;
		}
	}

	std::uint32_t UnmeasuredThisFrame()
	{
		return unmeasuredThisFrame;
	}

	float Visibility(RE::BSGeometry* a_geometry, RE::NiLight* a_light)
	{
		// A shape of a room merged beside the shown one is lit by that room's lights alone.
		const int slot = a_geometry ? Portal::Manager::MergedRoomOf(a_geometry) : -1;
		auto&     space = spaces[slot > 0 && slot < static_cast<int>(spaces.size()) ? slot : 0];
		if (!space.world || !a_geometry || !a_light) {
			return 1.0f;
		}
		const Key key{ a_geometry, a_light };
		const auto center = a_geometry->worldBound.center;
		if (const auto it = space.cache.find(key); it != space.cache.end() && it->second.center.GetSquaredDistance(center) < 25.0f * 25.0f) {
			return it->second.visibility;
		}
		RE::BSReadLockGuard lock(space.world->worldLock);
		if (LightOutside(space, a_light)) {
			return 0.0f;
		}
		if (raysThisFrame >= kRayBudgetPerFrame) {
			++unmeasuredThisFrame;
			return kUnmeasured;
		}
		const float visibility = Measure(space, a_geometry, a_light);
		space.cache[key] = { visibility, center };
		return visibility;
	}

	json Describe()
	{
		json bySlot = json::array();
		for (const auto& space : spaces) {
			bySlot.push_back({ { "pairs", space.cache.size() }, { "world", space.world != nullptr } });
		}
		return { { "pairs", spaces[0].cache.size() }, { "raysTotal", raysTotal }, { "world", spaces[0].world != nullptr }, { "slots", bySlot } };
	}
}

// The patched Community Shaders asks this for every shadow-casting light it gives an object drawn
// in a portal pass, and scales the light by it.
extern "C" __declspec(dllexport) float SeamlessPortal_LightVisibility(RE::BSGeometry* a_geometry, RE::NiLight* a_light)
{
	return Portal::LightOcclusion::Visibility(a_geometry, a_light);
}
