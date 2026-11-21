#pragma once

// Spike instrumentation, not shipping code. Its log answers the question the whole design hinges
// on: does the engine keep an interior cell resident (and its actors processed) after the player
// leaves it, and in which state? The answer is in docs/ENGINE.md, "Cells and spaces".
namespace Probe
{
	void Install();
	void DumpWorldState(std::string_view a_reason);
	std::string DescribeCell(RE::TESObjectCELL* a_cell);
}
