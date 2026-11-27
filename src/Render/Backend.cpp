#include "Render/Backend.h"

#include "Paths.h"
#include "Render/Enb.h"
#include "Render/Native.h"
#include "Render/ShaderData.h"

namespace
{
	using Render::Backend::Kind;

	Kind        kind = Kind::kNone;
	std::string why = "not installed";  // how the choice came about, for the log and dump_world
	bool        communityShadersLoaded = false;

	// The fork's exports (docs/COMMUNITY-SHADERS-PATCH.md); absent ones stay null.
	struct ForkApi
	{
		void (*setShaderData)(int, const float*, int) = nullptr;
		bool (*streetIBL)() = nullptr;
		void (*colorToWorking)(float*) = nullptr;
		void (*setInteriorSun)(bool) = nullptr;
		void (*settleSkylighting)() = nullptr;
		void (*cameraCut)() = nullptr;
		void (*setPortalShadows)(RE::BSShadowDirectionalLight*, void*, void*) = nullptr;
		void (*setPortalWorldspace)(RE::TESWorldSpace*) = nullptr;
		void (*setPortalCamera)(float, float) = nullptr;
		void (*beginPass)(RE::NiDirectionalLight*, bool) = nullptr;
		void (*endPass)() = nullptr;
		bool (*outdoorSunDirection)(float, const float*, float*) = nullptr;
		void (*gameLightColor)(const RE::NiLight*, float*) = nullptr;
		void (*setStreetSky)(RE::TESWorldSpace*, const float*) = nullptr;
	} fork;

	template <class T>
	void Find(HMODULE a_module, const char* a_name, T& a_out)
	{
		a_out = reinterpret_cast<T>(::GetProcAddress(a_module, a_name));
	}

	void FindFork(HMODULE a_module)
	{
		Find(a_module, "SeamlessPortal_SetShaderData", fork.setShaderData);
		Find(a_module, "SeamlessPortal_StreetIBL", fork.streetIBL);
		Find(a_module, "SeamlessPortal_ColorToWorking", fork.colorToWorking);
		Find(a_module, "SeamlessPortal_SetInteriorSun", fork.setInteriorSun);
		Find(a_module, "SeamlessPortal_SettleSkylighting", fork.settleSkylighting);
		Find(a_module, "SeamlessPortal_CameraCut", fork.cameraCut);
		Find(a_module, "SeamlessPortal_SetPortalShadows", fork.setPortalShadows);
		Find(a_module, "SeamlessPortal_SetPortalWorldspace", fork.setPortalWorldspace);
		Find(a_module, "SeamlessPortal_SetPortalCamera", fork.setPortalCamera);
		Find(a_module, "SeamlessPortal_BeginPass", fork.beginPass);
		Find(a_module, "SeamlessPortal_EndPass", fork.endPass);
		Find(a_module, "SeamlessPortal_OutdoorSunDirection", fork.outdoorSunDirection);
		Find(a_module, "SeamlessPortal_GameLightColor", fork.gameLightColor);
		Find(a_module, "SeamlessPortal_SetStreetSky", fork.setStreetSky);
	}

	// What was last set for the fork's shaders, built into Render::ShaderData at every change.
	struct Room
	{
		bool  enabled = false;
		float door[12]{};  // as SetMergedDoor takes it
		bool  hasLighting = false;
		float lighting[19]{};  // as SetMergedLighting takes it
		float daylight[3]{};
	};
	struct
	{
		Room  rooms[Render::Backend::kMaxMergedRooms];
		bool  hasFog = false;
		float fog[10]{};
		bool  hasFrontFog = false;
		float frontFog[10]{};
		float hostDaylight[3]{};
		float gatePassage = 0.0f;
		bool  interiorSun = false;
		float portalClip[4]{};
	} state;

	bool AnyRoom()
	{
		return std::ranges::any_of(state.rooms, [](const Room& a_room) { return a_room.enabled; });
	}

	// SeamlessSkyrim's colours are the game's. Community Shaders 2.0's Linear Lighting converts the
	// engine's own before the shaders see them, and these the same way.
	Render::Float4 Working(const float* a_rgb, float a_w)
	{
		float color[3]{ a_rgb[0], a_rgb[1], a_rgb[2] };
		if (fork.colorToWorking) {
			fork.colorToWorking(color);
		}
		return { color[0], color[1], color[2], a_w };
	}

	void Fog(const float* a_fog, Render::Float4& a_param, Render::Float4& a_near, Render::Float4& a_far)
	{
		const float range = std::max(a_fog[1] - a_fog[0], 1.0f);
		a_param = { a_fog[0] / range, 1.0f / range, a_fog[2], a_fog[3] };
		a_near = Working(a_fog + 4, 1.0f);
		a_far = Working(a_fog + 7, 0.0f);
	}

	// For the world's passes. Doorway centres stay in world coordinates: the shader makes them
	// camera-relative with the camera of the pass being drawn.
	Render::ShaderData WorldData()
	{
		Render::ShaderData data;
		if (!AnyRoom()) {
			return data;
		}
		const auto& first = state.rooms[0];
		const bool cameraInside = first.enabled && first.door[11] > 0.5f;
		const float streetLight = !state.interiorSun ? 0.0f : fork.streetIBL && fork.streetIBL() ? 2.0f : 1.0f;
		data.params = { 1.0f, first.enabled ? state.gatePassage : 0.0f, cameraInside ? 1.0f : 0.0f, streetLight };
		for (int slot = 0; slot < Render::Backend::kMaxMergedRooms; ++slot) {
			const auto&  room = state.rooms[slot];
			const float* door = room.door;
			data.doorPlane[slot] = { door[3], door[4], door[5], room.enabled ? 1.0f : 0.0f };
			data.doorCenter[slot] = { door[0], door[1], door[2], door[9] };
			data.doorRight[slot] = { door[6], door[7], door[8], door[10] };
			if (room.enabled && room.hasLighting) {
				const float* light = room.lighting;
				data.remoteDirColor[slot] = Working(light, 1.0f);
				data.remoteDirDirection[slot] = { light[3], light[4], light[5], light[18] };
				for (int row = 0; row < 3; ++row) {
					data.remoteAmbient[slot * 3 + row] = { light[6 + row * 4], light[7 + row * 4], light[8 + row * 4], light[9 + row * 4] };
				}
			}
			if (room.enabled) {
				data.roomDaylight[slot] = Working(room.daylight, 0.0f);
			}
		}
		if (first.enabled) {
			data.hostDaylight = Working(state.hostDaylight, 0.0f);
		}
		if (state.hasFog) {
			Fog(state.fog, data.remoteFogParam, data.remoteFogNear, data.remoteFogFar);
		}
		if (state.hasFrontFog) {
			Fog(state.frontFog, data.frontFogParam, data.frontFogNear, data.frontFogFar);
		}
		return data;
	}

	// For a portal pass, which draws one space: only room 0's doorway (the others' shapes aren't in
	// it), and the room as from inside it. A city gate's doorway is in the world, not in the space
	// the pass draws.
	Render::ShaderData PortalData()
	{
		Render::ShaderData data = WorldData();
		data.portalClip = { state.portalClip[0], state.portalClip[1], state.portalClip[2], state.portalClip[3] };
		for (int slot = 1; slot < Render::Backend::kMaxMergedRooms; ++slot) {
			data.doorPlane[slot].w = 0.0f;
		}
		std::ranges::fill(data.roomDaylight, Render::Float4{});
		data.hostDaylight = {};
		data.frontFogNear.w = 0.0f;
		if (state.rooms[0].enabled && state.gatePassage <= 0.0f) {
			data.params = { 1.0f, 0.0f, data.params.z, 0.0f };
		} else {
			data.doorPlane[0].w = 0.0f;
			data.params = {};
		}
		return data;
	}

	void Send(int a_view, const Render::ShaderData& a_data)
	{
		fork.setShaderData(a_view, &a_data.doorPlane[0].x, Render::kShaderDataFloat4s);
	}

	void SendWorld()
	{
		Send(0, WorldData());
	}

	std::string ReadChoice()
	{
		const auto ini = Paths::PluginDirectory() / "SeamlessSkyrim.ini";
		wchar_t    buffer[32]{};
		::GetPrivateProfileStringW(L"Render", L"sBackend", L"auto", buffer, static_cast<DWORD>(std::size(buffer)), ini.c_str());
		std::string choice;
		for (const wchar_t* c = buffer; *c; ++c) {
			choice += static_cast<char>(std::tolower(static_cast<unsigned char>(*c & 0x7F)));
		}
		return choice;
	}

	void UseNative(std::string a_why)
	{
		if (Render::Native::Install()) {
			kind = Kind::kNative;
			why = std::move(a_why);
		} else {
			kind = Kind::kNone;
			why = "the native backend could not hook the renderer";
		}
	}
}

namespace Render::Backend
{
	void Install()
	{
		Render::Enb::Detect();
		auto* communityShaders = ::GetModuleHandleW(L"CommunityShaders.dll");
		communityShadersLoaded = communityShaders != nullptr;
		if (communityShaders) {
			FindFork(communityShaders);
		}
		const bool isFork = fork.setShaderData != nullptr;
		// Before 0.8 the patch took each value through an export of its own.
		const bool oldFork = !isFork && fork.beginPass != nullptr;
		const auto choice = ReadChoice();
		if (choice == "cs" || choice == "communityshaders") {
			if (isFork) {
				kind = Kind::kCommunityShaders;
				why = "[Render] sBackend=cs";
			} else {
				why = oldFork          ? "[Render] sBackend=cs, but Community Shaders' SeamlessSkyrim patch is an older release's" :
				      communityShaders ? "[Render] sBackend=cs, but Community Shaders is not the SeamlessSkyrim fork" :
				                         "[Render] sBackend=cs, but Community Shaders isn't loaded";
			}
		} else if (choice == "native") {
			// Allowed with Community Shaders loaded, for testing: the mask doesn't depend on the
			// shaders, but Community Shaders lights the world its own way and ignores the lights
			// handed to a pass.
			UseNative(communityShaders ? "[Render] sBackend=native (Community Shaders is loaded: the room's light won't match)" : "[Render] sBackend=native");
		} else if (choice == "none" || choice == "off") {
			why = "[Render] sBackend=none";
		} else if (isFork) {
			kind = Kind::kCommunityShaders;
			why = "the SeamlessSkyrim fork of Community Shaders is loaded";
		} else if (communityShaders) {
			// It replaces the game's shaders; the native backend's lighting doesn't reach them, and
			// its mask has not been tried on them. sBackend=native tries anyway.
			why = oldFork ? "Community Shaders' SeamlessSkyrim patch is an older release's: install this release's" : "Community Shaders is loaded but is not the SeamlessSkyrim fork";
		} else {
			UseNative(Render::Enb::Present() ? "no Community Shaders; ENB runs on the game's own shaders" : "no Community Shaders: the game's own shaders");
		}
		logger::info("render: backend {} ({})", Name(), why);
	}

	bool GameShaders()
	{
		return !communityShadersLoaded;
	}

	Kind Current()
	{
		return kind;
	}

	const char* Name()
	{
		switch (kind) {
		case Kind::kCommunityShaders:
			return "Community Shaders (SeamlessSkyrim fork)";
		case Kind::kNative:
			return Render::Enb::Present() ? "native (ENB)" : "native";
		default:
			return "none";
		}
	}

	bool CanMerge()
	{
		return kind == Kind::kCommunityShaders || (kind == Kind::kNative && Render::Native::Ready());
	}

	int MergedRooms()
	{
		return kind == Kind::kCommunityShaders ? kMaxMergedRooms : 1;
	}

	bool AnyMerged()
	{
		return kind == Kind::kCommunityShaders && AnyRoom();
	}

	void SetMergedDoor(int a_room, const float* a_door)
	{
		if (a_room < 0 || a_room >= MergedRooms()) {
			return;
		}
		if (kind == Kind::kCommunityShaders) {
			auto& room = state.rooms[a_room];
			room.enabled = a_door != nullptr;
			if (a_door) {
				std::copy_n(a_door, std::size(room.door), room.door);
				// Only room 0 can hold the camera.
				room.door[11] = a_room == 0 ? a_door[11] : 0.0f;
			}
			SendWorld();
		} else if (kind == Kind::kNative) {
			Render::Native::SetDoor(a_door);
		}
	}

	void SetMergedLighting(int a_room, const float* a_lighting)
	{
		if (a_room < 0 || a_room >= MergedRooms()) {
			return;
		}
		if (kind == Kind::kCommunityShaders) {
			auto& room = state.rooms[a_room];
			room.hasLighting = a_lighting != nullptr;
			if (a_lighting) {
				std::copy_n(a_lighting, std::size(room.lighting), room.lighting);
			}
			SendWorld();
		} else if (kind == Kind::kNative) {
			Render::Native::SetMergedLighting(a_lighting);
		}
	}

	void SetMergedFog(const float* a_fog)
	{
		if (kind == Kind::kCommunityShaders) {
			state.hasFog = a_fog != nullptr;
			if (a_fog) {
				std::copy_n(a_fog, std::size(state.fog), state.fog);
			}
			SendWorld();
		} else if (kind == Kind::kNative) {
			Render::Native::SetMergedFog(a_fog);
		}
	}

	// Not done by the native backend: both need the pixel shader.
	void SetFrontFog(const float* a_fog)
	{
		if (kind == Kind::kCommunityShaders) {
			state.hasFrontFog = a_fog != nullptr;
			if (a_fog) {
				std::copy_n(a_fog, std::size(state.frontFog), state.frontFog);
			}
			SendWorld();
		}
	}

	void SetMergedDaylight(int a_room, const float* a_daylight)
	{
		if (kind == Kind::kCommunityShaders && a_room >= 0 && a_room < MergedRooms()) {
			std::ranges::fill(state.rooms[a_room].daylight, 0.0f);
			if (a_daylight) {
				std::copy_n(a_daylight, 3, state.rooms[a_room].daylight);
			}
			SendWorld();
		}
	}

	void SetHostDaylight(const float* a_daylight)
	{
		if (kind == Kind::kCommunityShaders) {
			std::ranges::fill(state.hostDaylight, 0.0f);
			if (a_daylight) {
				std::copy_n(a_daylight, 3, state.hostDaylight);
			}
			SendWorld();
		}
	}

	bool CanMaskGate()
	{
		return kind == Kind::kCommunityShaders || (kind == Kind::kNative && Render::Native::Ready());
	}

	void SetGatePassage(float a_depth)
	{
		if (kind == Kind::kCommunityShaders) {
			state.gatePassage = a_depth;
			SendWorld();
		} else if (kind == Kind::kNative) {
			Render::Native::SetGatePassage(a_depth);
		}
	}

	void SetStreetSky(RE::TESWorldSpace* a_worldspace, const RE::NiTransform& a_toInterior)
	{
		if (kind == Kind::kCommunityShaders && fork.setStreetSky) {
			// Rows of the rotation, then interior z = street z * scale + offset.
			float placement[11]{};
			for (int row = 0; row < 3; ++row) {
				for (int column = 0; column < 3; ++column) {
					placement[row * 3 + column] = a_toInterior.rotate.entry[row][column];
				}
			}
			placement[9] = a_toInterior.scale;
			placement[10] = a_toInterior.translate.z;
			fork.setStreetSky(a_worldspace, a_worldspace ? placement : nullptr);
		}
	}

	void SetInteriorSun(bool a_on)
	{
		if (kind == Kind::kCommunityShaders) {
			fork.setInteriorSun(a_on);
			state.interiorSun = a_on;
			SendWorld();
		} else if (kind == Kind::kNative) {
			Render::Native::SetStreetSun(a_on);
		}
	}

	void SettleSkylighting()
	{
		// The game's own lighting has nothing that builds up over time like CS's skylighting.
		if (kind == Kind::kCommunityShaders && fork.settleSkylighting) {
			fork.settleSkylighting();
		}
	}

	void CameraCut()
	{
		// Frame generation is Community Shaders' (FSR's): without it every frame is a rendered one.
		if (kind == Kind::kCommunityShaders && fork.cameraCut) {
			fork.cameraCut();
		}
	}

	void SetPortalClip(const float* a_plane)
	{
		if (kind == Kind::kCommunityShaders) {
			std::ranges::fill(state.portalClip, 0.0f);
			if (a_plane) {
				std::copy_n(a_plane, std::size(state.portalClip), state.portalClip);
			}
		} else if (kind == Kind::kNative) {
			Render::Native::SetPortalClip(a_plane);
		}
	}

	bool CanShadowPortal()
	{
		return kind == Kind::kCommunityShaders && fork.setPortalShadows;
	}

	void SetPortalShadows(RE::BSShadowDirectionalLight* a_light, void* a_shadowMaps, void* a_volumetricShadowMaps)
	{
		if (CanShadowPortal()) {
			fork.setPortalShadows(a_light, a_shadowMaps, a_volumetricShadowMaps);
		}
	}

	void SetPortalWorldspace(RE::TESWorldSpace* a_worldspace)
	{
		if (kind == Kind::kCommunityShaders && fork.setPortalWorldspace) {
			fork.setPortalWorldspace(a_worldspace);
		}
	}

	void BeginPortalPass(RE::NiDirectionalLight* a_sun, bool a_interior, const RE::NiCamera* a_camera)
	{
		if (kind == Kind::kCommunityShaders) {
			if (fork.setPortalCamera) {
				const auto& frustum = a_camera->GetRuntimeData2().viewFrustum;
				fork.setPortalCamera(frustum.fNear, frustum.fFar);
			}
			// The fork uploads its per-frame lighting once per frame, for the world: around the
			// portal pass it must describe the other side. The game's shaders read the sun and the
			// ambient the portal code already set, per draw.
			Send(1, PortalData());
			fork.beginPass(a_sun, a_interior);
		} else if (kind == Kind::kNative) {
			Render::Native::SetPortalPass(true);
		}
	}

	void EndPortalPass()
	{
		if (kind == Kind::kCommunityShaders) {
			fork.endPass();
		} else if (kind == Kind::kNative) {
			Render::Native::SetPortalPass(false);
		}
	}

	bool OutdoorSunDirection(float a_hour, const float* a_position, float* a_towardSun)
	{
		return kind == Kind::kCommunityShaders && fork.outdoorSunDirection && fork.outdoorSunDirection(a_hour, a_position, a_towardSun);
	}

	RE::NiColor GameLightColor(const RE::NiLight* a_light)
	{
		if (kind == Kind::kCommunityShaders && fork.gameLightColor) {
			RE::NiColor color;
			fork.gameLightColor(a_light, &color.red);
			return color;
		}
		return a_light->GetLightRuntimeData().diffuse;
	}

	bool CutsPortalShader(RE::BSShader::Type a_type)
	{
		switch (kind) {
		case Kind::kCommunityShaders:
			return a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Utility;
		case Kind::kNative:
			return a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Utility || a_type == RE::BSShader::Type::Effect;
		default:
			return false;
		}
	}

	bool MasksPrecipitation()
	{
		return kind == Kind::kCommunityShaders;
	}

	json Describe()
	{
		json out = {
			{ "backend", Name() },
			{ "why", why },
			{ "canMerge", CanMerge() },
			{ "communityShadersLoaded", communityShadersLoaded },
			{ "communityShadersFork", fork.setShaderData != nullptr },
			{ "mergedRooms", MergedRooms() },
			{ "masksGates", CanMaskGate() },
			{ "enb", Render::Enb::Describe() },
		};
		if (kind == Kind::kNative) {
			out["mask"] = Render::Native::DescribeMask();
			out["lighting"] = Render::Native::DescribeLighting();
		}
		return out;
	}
}

// Asked by the fork's Light Limit Fix for a merged room's lights: its doorway as SetMergedDoor took
// it; false while nothing is merged behind it.
extern "C" __declspec(dllexport) bool SeamlessPortal_MergedRoomDoor(int a_room, float* a_door)
{
	const auto& room = state.rooms[a_room];
	std::copy_n(room.door, std::size(room.door), a_door);
	return room.enabled;
}
