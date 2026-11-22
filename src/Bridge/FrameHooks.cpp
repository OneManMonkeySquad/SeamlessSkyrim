#include "Bridge/FrameHooks.h"

#include "Bridge/AgentMode.h"

#include "Portal/CameraLimit.h"
#include "Portal/PortalManager.h"
#include "Portal/PortalRender.h"
#include "Portal/Travel.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <directxtk/ScreenGrab.h>

namespace
{
	// ---- walk_to ---------------------------------------------------------------------------

	struct WalkState
	{
		FrameHooks::WalkRequest request;
		bool                    active = false;
		std::string             status = "idle";
		float                   elapsed = 0.0f;
		float                   stuckTime = 0.0f;
		RE::NiPoint3            lastPos;
	} walk;

	// Below this many units per second for a while we're walking into a wall or a closed door.
	constexpr float kStuckSpeed = 20.0f;
	constexpr float kStuckSeconds = 1.5f;

	void SteerPlayer(RE::PlayerCharacter* a_player, float a_delta)
	{
		auto* controls = RE::PlayerControls::GetSingleton();
		if (!walk.active || !controls) {
			return;
		}

		RE::NiPoint3 goal = walk.request.point;
		if (walk.request.target) {
			auto ref = walk.request.target.get();
			if (!ref) {
				FrameHooks::StopWalk("target reference no longer loaded");
				return;
			}
			goal = ref->GetPosition();
		}

		const auto  pos = a_player->GetPosition();
		const float dx = goal.x - pos.x;
		const float dy = goal.y - pos.y;
		const float distance = std::sqrt(dx * dx + dy * dy);
		walk.elapsed += a_delta;

		if (distance <= walk.request.stopDistance) {
			FrameHooks::StopWalk("arrived");
			return;
		}
		if (walk.elapsed >= walk.request.timeoutSeconds) {
			FrameHooks::StopWalk("timeout");
			return;
		}
		const float moved = std::hypot(pos.x - walk.lastPos.x, pos.y - walk.lastPos.y);
		walk.lastPos = pos;
		walk.stuckTime = (a_delta > 0.0f && moved / a_delta < kStuckSpeed) ? walk.stuckTime + a_delta : 0.0f;
		if (walk.stuckTime >= kStuckSeconds) {
			FrameHooks::StopWalk("stuck");
			return;
		}

		// Skyrim's yaw: 0 faces +Y (north), increasing clockwise.
		a_player->SetHeading(std::atan2(dx, dy));
		// Autorun is the engine's own "move forward" input, so movement goes through the normal
		// character controller, collision and animation instead of being faked.
		controls->data.autoMove = true;
		controls->data.running = walk.request.run;
	}

	// Diagnostics: wall-clock time between player updates (= frame time), last 120 frames.
	std::mutex                                         frameLock;
	std::array<float, 120>                             frameMs{};
	std::size_t                                        frameIndex = 0;
	std::optional<std::chrono::steady_clock::time_point> lastFrame;

	struct PlayerUpdateHook
	{
		static void Thunk(RE::PlayerCharacter* a_this, float a_delta)
		{
			{
				const auto now = std::chrono::steady_clock::now();
				std::scoped_lock lock(frameLock);
				if (lastFrame) {
					frameMs[frameIndex++ % frameMs.size()] = std::chrono::duration<float, std::milli>(now - *lastFrame).count();
				}
				lastFrame = now;
			}
			func(a_this, a_delta);
			SteerPlayer(a_this, a_delta);
			Portal::Manager::Update(a_delta);
			Portal::Travel::Update();
			const auto approach = Portal::Manager::CameraLimit();
			const auto leave = Portal::Travel::CameraLimit();
			Portal::CameraLimit::Set(approach && leave ? std::optional(std::min(*approach, *leave)) : approach ? approach : leave);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// ---- screenshot -------------------------------------------------------------------------

	struct CaptureRequest
	{
		std::wstring                       path;
		std::shared_ptr<std::promise<json>> done;
	};

	std::mutex                    captureLock;
	std::optional<CaptureRequest> pendingCapture;

	// CaptureFrames: copies on the GPU while taking them (a frame each), written out after.
	struct FrameSequence
	{
		std::wstring                                        directory;
		int                                                 left = 0;
		std::chrono::steady_clock::time_point               start;
		std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> frames;
		std::vector<double>                                 ms;
	};
	FrameSequence frameSequence;  // under captureLock

	void TakeSequenceFrame(IDXGISwapChain* a_swapChain)
	{
		auto& sequence = frameSequence;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		if (FAILED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) {
			sequence = {};
			return;
		}
		Microsoft::WRL::ComPtr<ID3D11Device>        device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		backBuffer->GetDevice(device.GetAddressOf());
		device->GetImmediateContext(context.GetAddressOf());
		D3D11_TEXTURE2D_DESC desc{};
		backBuffer->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
		if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, copy.GetAddressOf()))) {
			context->CopyResource(copy.Get(), backBuffer.Get());
			sequence.frames.push_back(copy);
			sequence.ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sequence.start).count());
		}
		if (--sequence.left > 0) {
			return;
		}
		const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		for (std::size_t i = 0; i < sequence.frames.size(); ++i) {
			const auto path = std::format(L"{}\\frame_{:02}_{:05.0f}.jpg", sequence.directory, i, sequence.ms[i]);
			DirectX::SaveWICTextureToFile(context.Get(), sequence.frames[i].Get(), GUID_ContainerFormatJpeg, path.c_str(), &GUID_WICPixelFormat24bppBGR);
		}
		if (SUCCEEDED(coInit)) {
			CoUninitialize();
		}
		logger::info("frames: {} saved to {}", sequence.frames.size(), std::filesystem::path(sequence.directory).string());
		sequence = {};
	}

	using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	Present_t originalPresent = nullptr;

	json Capture(IDXGISwapChain* a_swapChain, const std::wstring& a_path)
	{
		ID3D11Texture2D* backBuffer = nullptr;
		if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
			throw std::runtime_error("GetBuffer failed");
		}
		ID3D11Device*        device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		backBuffer->GetDevice(&device);
		device->GetImmediateContext(&context);

		D3D11_TEXTURE2D_DESC desc{};
		backBuffer->GetDesc(&desc);

		// WIC needs COM on this thread; an already-initialized apartment is fine either way.
		const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		// Force 24-bit BGR: the back buffer may be 10-bit or HDR, which JPEG can't hold.
		const HRESULT saved = DirectX::SaveWICTextureToFile(context, backBuffer, GUID_ContainerFormatJpeg, a_path.c_str(), &GUID_WICPixelFormat24bppBGR);
		if (SUCCEEDED(coInit)) {
			CoUninitialize();
		}

		context->Release();
		device->Release();
		backBuffer->Release();
		if (FAILED(saved)) {
			throw std::runtime_error(std::format("SaveWICTextureToFile failed (0x{:08X}, format {})", static_cast<std::uint32_t>(saved), static_cast<int>(desc.Format)));
		}
		return { { "width", desc.Width }, { "height", desc.Height }, { "format", static_cast<int>(desc.Format) } };
	}

	// ---- brightness log -------------------------------------------------------------------------

	struct BrightnessSample
	{
		std::string time;  // as the log prints it
		float       ms;    // since the frame before
		float       value;
		bool        arrival;  // the first frame drawn in a new space (MarkArrivalFrame)
		bool        held;     // drawn but not shown (GuardFlashes)
	};
	std::mutex                    brightnessLock;
	bool                          brightnessOn = false;
	std::vector<BrightnessSample> brightness;
	constexpr std::size_t         kMaxBrightnessSamples = 20000;

	// A few rows across the screen, copied to a staging texture and read two frames later, so the
	// GPU isn't waited for: three textures in turn.
	constexpr UINT kRows[] = { 4, 3, 2 };  // the row at height/kRows[i]
	struct Readback
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		std::string                     time;
		float                           ms = 0.0f;
		bool                            arrival = false;
		bool                            held = false;
		bool                            filled = false;
	};
	std::array<Readback, 3> readbacks;
	std::size_t             readbackIndex = 0;
	std::optional<std::chrono::steady_clock::time_point> lastPresent;

	float RowAverage(const D3D11_MAPPED_SUBRESOURCE& a_map, UINT a_width, DXGI_FORMAT a_format)
	{
		double sum = 0.0;
		for (UINT row = 0; row < std::size(kRows); ++row) {
			const auto* pixels = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(a_map.pData) + row * a_map.RowPitch);
			for (UINT x = 0; x < a_width; ++x) {
				const std::uint32_t p = pixels[x];
				if (a_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
					sum += ((p & 0x3FF) + ((p >> 10) & 0x3FF) + ((p >> 20) & 0x3FF)) / (3.0 * 1023.0);
				} else {
					sum += ((p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF)) / (3.0 * 255.0);
				}
			}
		}
		return static_cast<float>(sum / (a_width * std::size(kRows)));
	}

	std::atomic<bool> arrivalPresent = false;

	void SampleBrightness(IDXGISwapChain* a_swapChain, bool a_arrival, bool a_held)
	{
		const auto now = std::chrono::steady_clock::now();
		const float ms = lastPresent ? std::chrono::duration<float, std::milli>(now - *lastPresent).count() : 0.0f;
		lastPresent = now;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		backBuffer->GetDesc(&desc);
		const bool known = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
		                   desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
		if (!known || desc.SampleDesc.Count != 1) {
			logger::warn("brightness log: back buffer format {} ({} samples) not read; stopped", static_cast<int>(desc.Format), desc.SampleDesc.Count);
			brightnessOn = false;
			return;
		}
		Microsoft::WRL::ComPtr<ID3D11Device>        device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		backBuffer->GetDevice(device.GetAddressOf());
		device->GetImmediateContext(context.GetAddressOf());

		auto& slot = readbacks[readbackIndex++ % readbacks.size()];
		if (slot.filled) {
			D3D11_MAPPED_SUBRESOURCE map{};
			if (SUCCEEDED(context->Map(slot.texture.Get(), 0, D3D11_MAP_READ, 0, &map))) {
				const float value = RowAverage(map, desc.Width, desc.Format);
				context->Unmap(slot.texture.Get(), 0);
				std::scoped_lock lock(brightnessLock);
				if (brightness.size() < kMaxBrightnessSamples) {
					brightness.push_back({ slot.time, slot.ms, value, slot.arrival, slot.held });
				}
			}
		}
		D3D11_TEXTURE2D_DESC have{};
		if (slot.texture) {
			slot.texture->GetDesc(&have);
		}
		if (!slot.texture || have.Width != desc.Width || have.Format != desc.Format) {
			D3D11_TEXTURE2D_DESC staging{};
			staging.Width = desc.Width;
			staging.Height = static_cast<UINT>(std::size(kRows));
			staging.MipLevels = 1;
			staging.ArraySize = 1;
			staging.Format = desc.Format;
			staging.SampleDesc.Count = 1;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			slot.texture = nullptr;
			if (FAILED(device->CreateTexture2D(&staging, nullptr, slot.texture.GetAddressOf()))) {
				brightnessOn = false;
				return;
			}
		}
		for (UINT row = 0; row < std::size(kRows); ++row) {
			const UINT     y = desc.Height / kRows[row];
			const D3D11_BOX box{ 0, y, 0, desc.Width, y + 1, 1 };
			context->CopySubresourceRegion(slot.texture.Get(), 0, 0, row, 0, backBuffer.Get(), 0, &box);
		}
		const auto wall = std::chrono::system_clock::now();
		slot.time = std::format("{:%H:%M:%S}", std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::milliseconds>(wall)));
		slot.ms = ms;
		slot.arrival = a_arrival;
		slot.held = a_held;
		slot.filled = true;
	}

	// ---- flash guard ------------------------------------------------------------------------------
	// The first walk outdoors in a session drew one frame almost entirely white, two or three frames
	// after the crossing (the engine's own: also with this plugin's image-space blend off; behind a
	// loading screen it is never seen). For a short while after each crossing every frame's
	// brightness is read before it is shown, waiting for the GPU (a few rows, about a millisecond),
	// and a frame far brighter than the one shown before it is replaced by that one. Not by skipping
	// the Present: with Presents skipped the white lasted as many frames as were skipped (something
	// in the engine or a mod only moves on with each Present).
	constexpr float kFlashJump = 0.3f;  // of the average brightness, 0..1
	// Only a frame washed out to near white: a real step in brightness (the street after a dark room
	// on the first walk outdoors, 0.23 to 0.53) was held for six frames and then shown anyway.
	constexpr float kFlashBrightness = 0.8f;
	constexpr int   kMaxHeldInRow = 6;  // a scene really that bright is shown after this many
	std::atomic<int>                        guardFrames = 0;
	std::atomic<bool>                       guardReset = false;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> guardTexture;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> guardLastShown;  // copy of the last frame shown
	float                                   guardShown = -1.0f;  // brightness of the last frame shown
	int                                     guardHeld = 0;

	bool HoldFlash(IDXGISwapChain* a_swapChain)
	{
		if (guardReset.exchange(false)) {
			guardShown = -1.0f;
			guardHeld = 0;
		}
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
			return false;
		}
		D3D11_TEXTURE2D_DESC desc{};
		backBuffer->GetDesc(&desc);
		const bool known = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
		if (!known || desc.SampleDesc.Count != 1) {
			return false;
		}
		Microsoft::WRL::ComPtr<ID3D11Device>        device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		backBuffer->GetDevice(device.GetAddressOf());
		device->GetImmediateContext(context.GetAddressOf());
		D3D11_TEXTURE2D_DESC have{};
		if (guardTexture) {
			guardTexture->GetDesc(&have);
		}
		if (!guardTexture || have.Width != desc.Width || have.Format != desc.Format) {
			D3D11_TEXTURE2D_DESC staging{};
			staging.Width = desc.Width;
			staging.Height = static_cast<UINT>(std::size(kRows));
			staging.MipLevels = 1;
			staging.ArraySize = 1;
			staging.Format = desc.Format;
			staging.SampleDesc.Count = 1;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			guardTexture = nullptr;
			if (FAILED(device->CreateTexture2D(&staging, nullptr, guardTexture.GetAddressOf()))) {
				return false;
			}
		}
		for (UINT row = 0; row < std::size(kRows); ++row) {
			const UINT      y = desc.Height / kRows[row];
			const D3D11_BOX box{ 0, y, 0, desc.Width, y + 1, 1 };
			context->CopySubresourceRegion(guardTexture.Get(), 0, 0, row, 0, backBuffer.Get(), 0, &box);
		}
		D3D11_MAPPED_SUBRESOURCE map{};
		if (FAILED(context->Map(guardTexture.Get(), 0, D3D11_MAP_READ, 0, &map))) {
			return false;
		}
		const float value = RowAverage(map, desc.Width, desc.Format);
		context->Unmap(guardTexture.Get(), 0);
		if (guardShown >= 0.0f && guardLastShown && value - guardShown > kFlashJump && value > kFlashBrightness && guardHeld < kMaxHeldInRow) {
			++guardHeld;
			logger::info("flash guard: a frame of brightness {:.2f} after {:.2f} replaced by the one before", value, guardShown);
			context->CopyResource(backBuffer.Get(), guardLastShown.Get());
			return true;
		}
		D3D11_TEXTURE2D_DESC copyDesc{};
		if (guardLastShown) {
			guardLastShown->GetDesc(&copyDesc);
		}
		if (!guardLastShown || copyDesc.Width != desc.Width || copyDesc.Height != desc.Height || copyDesc.Format != desc.Format) {
			D3D11_TEXTURE2D_DESC copy = desc;
			copy.Usage = D3D11_USAGE_DEFAULT;
			copy.BindFlags = 0;
			copy.CPUAccessFlags = 0;
			copy.MiscFlags = 0;
			guardLastShown = nullptr;
			if (FAILED(device->CreateTexture2D(&copy, nullptr, guardLastShown.GetAddressOf()))) {
				guardLastShown = nullptr;
			}
		}
		if (guardLastShown) {
			context->CopyResource(guardLastShown.Get(), backBuffer.Get());
		}
		guardShown = value;
		guardHeld = 0;
		return false;
	}

	HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
	{
		const bool arrival = arrivalPresent.exchange(false);
		bool       held = false;
		if (guardFrames > 0) {
			--guardFrames;
			held = HoldFlash(a_swapChain);
		}
		if (brightnessOn) {
			SampleBrightness(a_swapChain, arrival, held);
		}
		std::optional<CaptureRequest> request;
		{
			std::scoped_lock lock(captureLock);
			request.swap(pendingCapture);
			if (frameSequence.left > 0) {
				TakeSequenceFrame(a_swapChain);
			}
		}
		if (request) {
			// Before Present: afterwards a flip-model back buffer's contents are undefined.
			try {
				request->done->set_value(Capture(a_swapChain, request->path));
			} catch (...) {
				request->done->set_exception(std::current_exception());
			}
		}
		return originalPresent(a_swapChain, a_syncInterval, a_flags);
	}

	void InstallPresentHook()
	{
		auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
		auto* swapChain = window ? reinterpret_cast<IDXGISwapChain*>(window->swapChain) : nullptr;
		if (!swapChain) {
			logger::error("frame hooks: no swap chain, screenshots disabled");
			return;
		}
		// Patching the shared vtable chains with any other Present hook (Community Shaders, ENB
		// helpers): we call whatever was installed before us.
		auto*           vtable = *reinterpret_cast<std::uintptr_t**>(swapChain);
		constexpr auto kPresentIndex = 8;
		originalPresent = reinterpret_cast<Present_t>(vtable[kPresentIndex]);
		REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtable[kPresentIndex]), reinterpret_cast<std::uintptr_t>(&PresentThunk));
	}
}

namespace FrameHooks
{
	void Install()
	{
		// The player update drives the portals and the agent's walking; with neither there is
		// nothing for it to do (Portal::Manager::Install runs first and decides about portals).
		if (!AgentMode::Enabled() && !Portal::Manager::HooksInstalled()) {
			logger::info("frame hooks: none needed");
			return;
		}
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdateHook::func = vtable.write_vfunc(0xAD, PlayerUpdateHook::Thunk);
		// Only when something needs it (agent screenshots, portal compositing): installed during
		// normal play, the Present hook sat next to CS's frame-generation proxy and the image came
		// out blurred/doubled even with portals off.
		if (AgentMode::Enabled() || Portal::Manager::Enabled()) {
			EnsurePresentHook();
		}
		logger::info("frame hooks installed (Present hook: {})", originalPresent != nullptr);
	}

	void EnsurePresentHook()
	{
		if (!originalPresent) {
			InstallPresentHook();
		}
	}

	void StartWalk(const WalkRequest& a_request)
	{
		walk = {};
		walk.request = a_request;
		walk.active = true;
		walk.status = "walking";
		walk.lastPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
	}

	void StopWalk(std::string_view a_reason)
	{
		if (walk.active) {
			if (auto* controls = RE::PlayerControls::GetSingleton()) {
				controls->data.autoMove = false;
			}
		}
		walk.active = false;
		walk.status = a_reason;
	}

	json FrameStats()
	{
		std::scoped_lock lock(frameLock);
		const auto       count = std::min(frameIndex, frameMs.size());
		if (!count) {
			return nullptr;
		}
		std::vector<float> sorted(frameMs.begin(), frameMs.begin() + count);
		std::ranges::sort(sorted);
		const float sum = std::accumulate(sorted.begin(), sorted.end(), 0.0f);
		return { { "frames", count }, { "avgMs", sum / count }, { "medianMs", sorted[count / 2] }, { "p95Ms", sorted[count * 95 / 100] }, { "maxMs", sorted.back() } };
	}

	json WalkStatus()
	{
		return { { "active", walk.active }, { "status", walk.status }, { "elapsed", walk.elapsed } };
	}

	bool CaptureFrames(const std::wstring& a_directory, int a_frames)
	{
		std::scoped_lock lock(captureLock);
		if (!originalPresent) {
			return false;
		}
		// Takes over from a run still being taken, whose frames are dropped.
		frameSequence = { a_directory, a_frames, std::chrono::steady_clock::now() };
		return true;
	}

	// Main thread only, like the crossings and merges that read them.
	std::optional<std::wstring> eventCaptureRoot;
	int                         eventCaptureCount = 0;

	void SetEventCapture(const std::wstring& a_directory)
	{
		eventCaptureRoot = a_directory;
		eventCaptureCount = 0;
	}

	void CaptureEventFrames(std::wstring_view a_event, int a_frames)
	{
		if (!eventCaptureRoot) {
			wchar_t directory[MAX_PATH];
			eventCaptureRoot = AgentMode::Enabled() && ::GetEnvironmentVariableW(L"SEAMLESS_TRAVEL_CAPTURE", directory, MAX_PATH) > 0 ? std::wstring(directory) : std::wstring();
		}
		if (eventCaptureRoot->empty()) {
			return;
		}
		{
			// A merge comes a few hundred ms into its crossing's frames. Starting anew there threw
			// the crossing's frames away, every time; one run of frames shows both.
			std::scoped_lock lock(captureLock);
			if (frameSequence.left > 0) {
				constexpr int kMostFrames = 60;  // each is a copy of the back buffer until written
				frameSequence.left = std::max(frameSequence.left, std::min(a_frames, kMostFrames - static_cast<int>(frameSequence.frames.size())));
				return;
			}
		}
		const auto directory = std::format(L"{}\\{}_{:02}", *eventCaptureRoot, a_event, ++eventCaptureCount);
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		CaptureFrames(directory, a_frames);
	}

	void GuardFlashes(int a_frames)
	{
		guardReset = true;
		guardFrames = a_frames;
	}

	void MarkArrivalFrame()
	{
		arrivalPresent = true;
	}

	void RecordBrightness(bool a_start)
	{
		std::scoped_lock lock(brightnessLock);
		if (a_start) {
			brightness.clear();
			for (auto& slot : readbacks) {
				slot.filled = false;
			}
			lastPresent.reset();
		}
		brightnessOn = a_start;
	}

	json BrightnessLog()
	{
		std::scoped_lock lock(brightnessLock);
		json rows = json::array();
		for (const auto& s : brightness) {
			rows.push_back({ s.time, std::round(s.ms * 10.0f) / 10.0f, std::round(s.value * 1000.0f) / 1000.0f, s.arrival, s.held });
		}
		return { { "recording", brightnessOn }, { "frames", rows } };
	}

	json CaptureScreenshot(const std::wstring& a_path, std::chrono::milliseconds a_timeout)
	{
		if (!originalPresent) {
			throw std::runtime_error("screenshot hook not installed");
		}
		auto done = std::make_shared<std::promise<json>>();
		auto future = done->get_future();
		{
			std::scoped_lock lock(captureLock);
			if (pendingCapture) {
				throw std::runtime_error("a screenshot is already pending");
			}
			pendingCapture = CaptureRequest{ a_path, done };
		}
		if (future.wait_for(a_timeout) != std::future_status::ready) {
			std::scoped_lock lock(captureLock);
			pendingCapture.reset();
			throw std::runtime_error("no frame presented in time (game minimized?)");
		}
		return future.get();
	}
}
