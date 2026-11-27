#pragma once

// A water reflection (TESWaterReflections, shared by every water at its height) keeps a plain
// pointer to the material of the water it was made for (+0x40, set by its constructor and never
// again), and TESWaterSystem::AddWater reads it when another water joins (ID 32179 +0x685). The
// reflection outlives that water whenever others have joined it: the street's water added while
// the player is indoors joins the room's reflection (Riften's jail walked out of, then the Bee
// and Barb walked into, read memory that had become a BSTriShape), and it happens across a
// loading screen too, where nothing of the plugin runs (the sea's reflection at Winterhold).
// Each reflection takes a reference on its material when it is made and gives it back when it is
// destroyed.
namespace Portal::WaterReflections
{
	void Install();
}
