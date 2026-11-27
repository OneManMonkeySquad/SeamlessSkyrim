#include "Portal/WaterReflections.h"

#include "Hooks.h"

namespace Portal::WaterReflections
{
	namespace
	{
		struct Held
		{
			const RE::TESWaterReflections* reflection;
			RE::BSShaderMaterial*          material;
		};
		std::vector<Held> held;
		// AddLODWater makes reflections in the LOD job, on a worker thread.
		std::mutex heldLock;

		// The shader material manager and its lock (taken by its lookup and release, IDs 107719 /
		// 107720).
		REL::Relocation<void**>          materialManager{ REL::RelocationID(517047, 403555) };
		REL::Relocation<RE::BSSpinLock*> materialLock{ REL::RelocationID(528338, 415284) };

		struct ConstructHook
		{
			static RE::TESWaterReflections* Thunk(RE::TESWaterReflections* a_this, void* a_plane, RE::BSShaderMaterial* a_material, std::uint16_t a_flags, void* a_world)
			{
				auto* reflection = func(a_this, a_plane, a_material, a_flags, a_world);
				{
					// A shared material's count only changes under the manager's lock: its release
					// compares the count with zero after decrementing it.
					RE::BSSpinLockGuard guard(*materialLock);
					a_material->IncRef();
				}
				std::scoped_lock guard(heldLock);
				assert(std::ranges::find(held, reflection, &Held::reflection) == held.end());
				held.push_back({ reflection, a_material });
				return reflection;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		struct DestroyHook
		{
			static void* Thunk(RE::TESWaterReflections* a_this, std::uint32_t a_flags)
			{
				RE::BSShaderMaterial* material = nullptr;
				{
					std::scoped_lock guard(heldLock);
					const auto       it = std::ranges::find(held, a_this, &Held::reflection);
					material = it->material;
					held.erase(it);
				}
				auto* result = func(a_this, a_flags);
				// The engine's release, not the material's destructor: at the last reference it takes
				// the material out of the manager's table first. Deleted directly, the entry stays and
				// the next lookup with its key calls into freed memory.
				static REL::Relocation<void (*)(void*, RE::BSShaderMaterial*)> release{ REL::RelocationID(100935, 107720) };
				release(*materialManager, material);
				return result;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	void Install()
	{
		const REL::RelocationID         kAddWater(31388, 32179), kConstruct(31371, 32158);
		REL::Relocation<std::uintptr_t> site{ kAddWater, REL::Relocate(0x66C, 0x770) };
		if (!Hooks::CallsTo(site.address(), kConstruct, "water reflections: the constructor")) {
			logger::warn("water reflections: not installed; a street's water joining a room's reflection can crash later");
			return;
		}
		ConstructHook::func = SKSE::GetTrampoline().write_call<5>(site.address(), ConstructHook::Thunk);
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_TESWaterReflections[0] };
		DestroyHook::func = vtable.write_vfunc(0x0, DestroyHook::Thunk);
		logger::info("water reflections: installed");
	}
}
