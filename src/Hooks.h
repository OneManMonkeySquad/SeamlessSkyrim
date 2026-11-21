#pragma once

#include <Windows.h>

namespace Hooks
{
	// Whether the call at a_site, which we are about to redirect, is the one we reversed: a
	// relative call to Address Library a_id. Checking only for the call opcode accepted any call
	// at that spot (a different game build, a site moved by another mod's patch).
	//
	// A call that leads out of SkyrimSE.exe is another mod's hook on the same call (Community
	// Shaders has some of these sites: RenderWorld, SetupAndDrawPass). It is accepted: ours then
	// goes in front of it and calls it in place of the engine's function.
	inline bool CallsTo(std::uintptr_t a_site, std::uint64_t a_id, std::string_view a_what)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_site);
		if (code[0] != 0xE8) {
			logger::error("hooks: {}: no call at the hook site", a_what);
			return false;
		}
		const auto target = a_site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
		if (target == REL::ID(a_id).address()) {
			return true;
		}
		static const auto [begin, end] = [] {
			const auto  base = REL::Module::get().base();
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			return std::pair{ base, base + nt->OptionalHeader.SizeOfImage };
		}();
		if (target < begin || target >= end) {
			logger::info("hooks: {}: the call is already redirected by another mod; chained", a_what);
			return true;
		}
		logger::error("hooks: {}: the call goes to {:X} instead of ID {}", a_what, target - begin, a_id);
		return false;
	}
}
