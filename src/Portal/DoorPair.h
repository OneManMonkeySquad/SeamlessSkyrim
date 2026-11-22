#pragma once

#include "Portal/PortalManager.h"

// Geometry of a load door pair: where the doorway is and how space in front of the source door
// maps to space in front of the linked door. Shared by the renderer (portal camera, quad) and the
// travel code (where the player comes out).
namespace Portal::Doors
{
	// The door reference, or throws if it has no 3D (not loaded).
	RE::TESObjectREFR* Door(RE::FormID a_id);

	// The door panel's corners in its cell's space (closed, as measured from the mesh: the leaves
	// its opening animation moves, between their faces). Corner index bits: 1 = max x, 2 = max y,
	// 4 = max z, in the door's model space.
	std::array<RE::NiPoint3, 8> Corners(RE::TESObjectREFR* a_door);
	RE::NiPoint3                Center(RE::TESObjectREFR* a_door);

	struct PanelSize
	{
		float halfWidth = 0;
		float halfDepth = 0;  // half the panel's thickness
		float halfHeight = 0;
	};
	[[nodiscard]] PanelSize Size(RE::TESObjectREFR* a_door);

	// The doorway as a flat quad just off the door panel (as measured closed): in front of it on
	// the viewer's side, or with a_behindPanel just behind its far face. a_wholeMesh: as wide and
	// high as all of the door's mesh (a frame included), not only its leaves.
	std::array<RE::NiPoint3, 4> Quad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, bool a_behindPanel = false, bool a_wholeMesh = false);
	// The same where the door is drawn: a cell merged into the player's space has its 3D moved,
	// and a picture or a swing direction in its own coordinates landed at the cell's unmoved place.
	// a_viewer is in the player's space. Identical to Quad and Center for any other door.
	std::array<RE::NiPoint3, 4> ShownQuad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, bool a_behindPanel = false, bool a_wholeMesh = false);
	RE::NiPoint3                ShownCenter(RE::TESObjectREFR* a_door);
	// The door's cell's own coordinates to where it is drawn (identity-like for an unmoved cell).
	RE::NiTransform             ToShown(RE::TESObjectREFR* a_door);

	struct Mapping
	{
		RE::NiTransform toLinked;      // source side -> linked side
		RE::NiPoint3    sourceCenter;  // middle of the source door panel
		RE::NiPoint3    inward;        // horizontal unit vector pointing through the source door
		float           yawDelta = 0;  // how much a heading turns when mapped
		float           halfWidth = 0; // half the doorway's width
		float           halfDepth = 0; // half the door's thickness: a city gate's is a step's worth
		float           halfHeight = 0;
	};

	// Throws if a door has no 3D or no teleport marker.
	Mapping Compute(const Manager::Active& a_portal);
	// Nothing while a door of the pair has no 3D: loading, or unloaded under a portal that is about
	// to be let go. Every frame's work on a door pair waits for both doors this way.
	std::optional<Mapping> TryCompute(const Manager::Active& a_portal);

	// Some interiors are a little turned against the outside they belong to (the doors' markers
	// don't quite agree); the player can correct that by hand (the in-game menu). The correction
	// belongs to the pair and is stored under one of its doors, the outdoor one where there is
	// one: degrees the inside is turned around its door, saved in SeamlessSkyrim_Doors.ini.
	[[nodiscard]] RE::TESObjectREFR* CorrectionDoor(RE::TESObjectREFR* a_source, RE::TESObjectREFR* a_linked);
	// The same by IDs and the sides' kinds: the linked door needn't be loaded.
	[[nodiscard]] RE::FormID CorrectionDoor(RE::FormID a_source, bool a_sourceOutdoors, RE::FormID a_linked, bool a_linkedOutdoors);
	[[nodiscard]] float              Correction(RE::FormID a_door);
	void                             SetCorrection(RE::FormID a_door, float a_degrees);
	// Everything the menu sets for the pair (turn, shift, hinge and the switches) at once: the
	// menu applies each live and keeps it only on "Save for this door", so trying a setting out
	// doesn't change the file.
	void                             SaveCorrection(RE::FormID a_door);
	// The inside moved against the outside, in the outdoor door's frame: x to the right (seen
	// walking in), y further in, z up; in game units. Stored with the turn.
	[[nodiscard]] RE::NiPoint3       Shift(RE::FormID a_door);
	void                             SetShift(RE::FormID a_door, const RE::NiPoint3& a_shift);

	// How the pair's door opens, chosen per pair in the menu (stored with the correction): which
	// end is hinged can only be guessed from the mesh.
	enum class OpenStyle : int
	{
		kSwing = 0,          // turns on the end the mesh guess picked
		kSwingOtherEnd = 1,  // turns on the other end
	};
	[[nodiscard]] OpenStyle Style(RE::FormID a_door);
	void                    SetStyle(RE::FormID a_door, OpenStyle a_style);

	// The pair's portal switched off (the menu, stored with the correction): an ordinary door
	// again, with its loading screen. Either door of the pair may be passed.
	[[nodiscard]] bool PortalOff(RE::FormID a_door);
	void               SetPortalOff(RE::FormID a_door, bool a_off);

	// Which way the pair's door opens (the menu, stored with the correction): outwards, into the
	// street, unless set to open inwards (between two interiors "outwards" is the correction
	// door's side). The same seen from either side: of the two doors standing in the doorway, the
	// one in the space it opens into is swung (SwingingDoor), the other hidden while it is open.
	[[nodiscard]] bool       OpensInward(RE::FormID a_door);
	void                     SetOpensInward(RE::FormID a_door, bool a_inward);
	[[nodiscard]] RE::FormID SwingingDoor(RE::FormID a_source, bool a_sourceOutdoors, RE::FormID a_linked, bool a_linkedOutdoors);

	// The pair's doorway stands open whatever the player's distance (the menu, stored with the
	// correction), shown by its picture while the space behind isn't there (Snapshot).
	[[nodiscard]] bool                    AlwaysOpen(RE::FormID a_door);
	void                                  SetAlwaysOpen(RE::FormID a_door, bool a_open);
	[[nodiscard]] std::vector<RE::FormID> AlwaysOpenDoors();

	// A door standing open for a portal: its collision off (the collision objects sit on child
	// nodes of the door's 3D, so each one is switched) and optionally not drawn.
	// a_passable: its bodies out of the Havok world as well. The layer change leaves a city gate
	// solid, which is right only while there is nothing to walk through to.
	// a_solid: its collision kept (a doorway shown open before the space behind it is there).
	struct OpenState
	{
		RE::FormID door = 0;
		bool       hidden = false;
		bool       passable = false;
		bool       solid = false;
	};
	void Open(RE::TESObjectREFR* a_door, OpenState& a_state, bool a_hide, bool a_passable = false, bool a_solid = false);
	void Close(OpenState& a_state);
	// The flags an open door carries (no prompt, no ordinary trip through it) off or back on, for
	// every door held open: they are stored with a reference, and must not go into a save made
	// while doors stay open (walking in a room walked into, Manager::NoteRoomForSave).
	void SetOpenFlags(bool a_set);

	// Main thread: the door's leaves swung open around their hinges, a_amount from 0 (closed) to 1
	// (open), towards a_room (a horizontal world direction: the side it opens into); its frame
	// stays. Placement and everything derived from it keep using the closed door.
	void Swing(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_room, float a_amount, bool a_otherEnd = false);
	// Main thread: the panel back where it was.
	void Unswing(RE::FormID a_door);

	// Main thread, now and then: forgets the measurements of doors whose 3D has gone. The cache holds
	// the 3D it measured, and kept every door ever approached in memory with its mesh.
	void ForgetUnloaded();

	// Main thread: the door is held open by a portal right now.
	[[nodiscard]] bool IsOpen(RE::FormID a_door);
	// Main thread: the door is held open passable (Open), its collision out of the Havok world.
	[[nodiscard]] bool BodiesHidden(RE::FormID a_door);

	// Once at startup: an open doorway shows no "Open ..." prompt.
	void InstallPromptHook();

	// Main thread, every frame: takes the "Open ..." prompt off the HUD while the crosshair is
	// on a door a portal holds open. Returns how often the engine asked for a door's text.
	std::uint32_t HidePrompt();
}