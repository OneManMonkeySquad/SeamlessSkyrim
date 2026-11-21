#include "Bridge/AgentMode.h"

#include <Windows.h>

namespace
{
	decltype(&::SetForegroundWindow) originalSetForegroundWindow = nullptr;
	decltype(&::SetFocus)            originalSetFocus = nullptr;
	decltype(&::ShowWindow)          originalShowWindow = nullptr;
	decltype(&::SetWindowPos)        originalSetWindowPos = nullptr;
	decltype(&::CreateWindowExW)     originalCreateWindowExW = nullptr;
	decltype(&::CreateWindowExA)     originalCreateWindowExA = nullptr;

	BOOL WINAPI GuardedSetForegroundWindow(HWND)
	{
		return TRUE;  // pretend it worked; the game doesn't need to be in front to run
	}

	HWND WINAPI GuardedSetFocus(HWND a_window)
	{
		// SetFocus on a top-level window activates it. Report the window as focused instead.
		return a_window;
	}

	int NonActivating(int a_command)
	{
		switch (a_command) {
		case SW_SHOWNORMAL:
		case SW_RESTORE:
		case SW_SHOWDEFAULT:
			return SW_SHOWNOACTIVATE;
		case SW_SHOW:
			return SW_SHOWNA;
		default:
			return a_command;
		}
	}

	BOOL WINAPI GuardedShowWindow(HWND a_window, int a_command)
	{
		return originalShowWindow(a_window, NonActivating(a_command));
	}

	BOOL WINAPI GuardedSetWindowPos(HWND a_window, HWND a_after, int a_x, int a_y, int a_cx, int a_cy, UINT a_flags)
	{
		return originalSetWindowPos(a_window, a_after, a_x, a_y, a_cx, a_cy, a_flags | SWP_NOACTIVATE);
	}

	// A top-level window created with WS_VISIBLE is shown *and activated*; create it hidden and
	// show it without activation instead.
	template <class Char, class Original>
	HWND CreateHidden(Original a_original, DWORD a_exStyle, const Char* a_class, const Char* a_name, DWORD a_style, int a_x, int a_y, int a_w, int a_h, HWND a_parent, HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
	{
		const bool topLevelVisible = !a_parent && (a_style & WS_VISIBLE);
		const HWND window = a_original(a_exStyle, a_class, a_name, topLevelVisible ? a_style & ~WS_VISIBLE : a_style, a_x, a_y, a_w, a_h, a_parent, a_menu, a_instance, a_param);
		if (window && topLevelVisible) {
			originalShowWindow(window, SW_SHOWNA);
		}
		return window;
	}

	HWND WINAPI GuardedCreateWindowExW(DWORD a_exStyle, LPCWSTR a_class, LPCWSTR a_name, DWORD a_style, int a_x, int a_y, int a_w, int a_h, HWND a_parent, HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
	{
		return CreateHidden(originalCreateWindowExW, a_exStyle, a_class, a_name, a_style, a_x, a_y, a_w, a_h, a_parent, a_menu, a_instance, a_param);
	}

	HWND WINAPI GuardedCreateWindowExA(DWORD a_exStyle, LPCSTR a_class, LPCSTR a_name, DWORD a_style, int a_x, int a_y, int a_w, int a_h, HWND a_parent, HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
	{
		return CreateHidden(originalCreateWindowExA, a_exStyle, a_class, a_name, a_style, a_x, a_y, a_w, a_h, a_parent, a_menu, a_instance, a_param);
	}

	template <class F>
	void Patch(F& a_original, F a_replacement, const char* a_function)
	{
		// Chains with any earlier IAT hook (other mods patch window creation too).
		const auto previous = SKSE::PatchIAT(a_replacement, "user32.dll", a_function);
		a_original = previous ? reinterpret_cast<F>(previous) : reinterpret_cast<F>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), a_function));
		logger::info("agent mode: guarded {} ({})", a_function, previous ? "IAT" : "not imported, resolved directly");
	}
}

namespace AgentMode
{
	namespace
	{
		bool EnvFlag(const char* a_name)
		{
			char       value[8]{};
			const auto length = ::GetEnvironmentVariableA(a_name, value, sizeof(value));
			return length > 0 && value[0] == '1';
		}
	}

	bool Enabled()
	{
		static const bool enabled = EnvFlag("SEAMLESS_AGENT");
		return enabled;
	}

	bool Background()
	{
		static const bool background = EnvFlag("SEAMLESS_AGENT_BACKGROUND");
		return background;
	}

	void InstallFocusGuards()
	{
		if (!Background()) {
			return;
		}
		Patch(originalShowWindow, &GuardedShowWindow, "ShowWindow");
		Patch(originalSetForegroundWindow, &GuardedSetForegroundWindow, "SetForegroundWindow");
		Patch(originalSetFocus, &GuardedSetFocus, "SetFocus");
		Patch(originalSetWindowPos, &GuardedSetWindowPos, "SetWindowPos");
		Patch(originalCreateWindowExW, &GuardedCreateWindowExW, "CreateWindowExW");
		Patch(originalCreateWindowExA, &GuardedCreateWindowExA, "CreateWindowExA");
	}

	namespace
	{
		bool GameHasFocus()
		{
			DWORD process = 0;
			::GetWindowThreadProcessId(::GetForegroundWindow(), &process);
			return process == ::GetCurrentProcessId();
		}

		template <class Device>
		struct PollHook
		{
			static void Thunk(Device* a_device, float a_delta)
			{
				if (GameHasFocus()) {
					func(a_device, a_delta);
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	void InstallInputGuards()
	{
		if (!Background()) {
			return;
		}
		// With bAlwaysActive the game keeps reading keyboard and mouse while it sits unfocused on
		// its own desktop: whatever the user typed elsewhere opened the inventory and the map,
		// walked the player into doors and paused test runs. Only read input while the game window
		// really is in front (the MCP server focuses it for its own key presses).
		REL::Relocation<std::uintptr_t> keyboard{ RE::VTABLE_BSWin32KeyboardDevice[0] };
		PollHook<RE::BSWin32KeyboardDevice>::func = keyboard.write_vfunc(0x2, PollHook<RE::BSWin32KeyboardDevice>::Thunk);
		REL::Relocation<std::uintptr_t> mouse{ RE::VTABLE_BSWin32MouseDevice[0] };
		PollHook<RE::BSWin32MouseDevice>::func = mouse.write_vfunc(0x2, PollHook<RE::BSWin32MouseDevice>::Thunk);
		logger::info("agent mode: input only while focused");
	}

	namespace
	{
		class AutoLoadSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			std::string save;

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME && !save.empty()) {
					logger::info("agent mode: loading '{}' at the main menu", save);
					RE::BGSSaveLoadManager::GetSingleton()->Load(std::exchange(save, std::string()).c_str());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} autoLoad;
	}

	void InstallAutoLoad()
	{
		char value[260]{};
		if (!Enabled() || ::GetEnvironmentVariableA("SEAMLESS_AGENT_LOAD", value, sizeof(value)) == 0 || !value[0]) {
			return;
		}
		autoLoad.save = value;
		auto* ui = RE::UI::GetSingleton();
		// The main menu is usually up already when the game data has finished loading (kDataLoaded):
		// its opening was missed, and the game sat in it. Loaded on the next frame then.
		if (ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
			SKSE::GetTaskInterface()->AddTask([] {
				if (!autoLoad.save.empty()) {
					logger::info("agent mode: loading '{}' (main menu already up)", autoLoad.save);
					RE::BGSSaveLoadManager::GetSingleton()->Load(std::exchange(autoLoad.save, std::string()).c_str());
				}
			});
		} else if (ui) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&autoLoad);
		}
	}
}
