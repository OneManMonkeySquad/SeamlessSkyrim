#pragma once

struct ID3D11ShaderResourceView;

// The native backend (see Render/Backend.h): seamless rendering with the game's own shaders.
namespace Render::Native
{
	// Hooks the shaders' per-draw setup and the device context's draw calls. False if something
	// it needs isn't there (then the backend is off).
	bool Install();
	[[nodiscard]] bool Ready();

	// NativeMask.cpp: the doorway mask and the portal pass's far-door clip.
	void SetDoor(const float* a_door);
	void SetGatePassage(float a_depth);
	void SetPortalClip(const float* a_plane);
	void SetPortalPass(bool a_active);
	[[nodiscard]] json DescribeMask();

	// NativePortalShadows.cpp: the sun's shadows of the portal pass, as a shadow mask of its own built
	// from its depth (after its depth prepass) and the shadow maps drawn for its camera.
	struct PortalShadowInputs
	{
		ID3D11ShaderResourceView*         depth;
		ID3D11ShaderResourceView*         shadowMaps;
		RE::BSShadowDirectionalLight*     light;
		const RE::BSGraphics::ViewData*   view;       // the renderer's camera data the pass drew with
		RE::NiPoint3                      posAdjust;  // ... and its camera-relative origin
		float                             pictureWidth;
		float                             pictureHeight;
		std::uint32_t                     targetWidth;
		std::uint32_t                     targetHeight;
		float                             bias;
	};
	void BuildPortalShadowMask(const PortalShadowInputs& a_in);
	void EndPortalShadowMask();
	// The mask the portal pass's lighting draws read (PS t14) while it is drawn; nullptr: none.
	[[nodiscard]] ID3D11ShaderResourceView* PortalShadowMask();

	// NativeLighting.cpp: the light of the merged space and of the portal pass.
	void SetMergedLighting(const float* a_lighting);
	void SetMergedFog(const float* a_fog);
	// The street merged into the interior: the engine draws the sun's shadows as outdoors.
	void SetStreetSun(bool a_on);
	[[nodiscard]] bool StreetSunOn();
	void InstallLightingHooks();
	[[nodiscard]] bool MergedDoorSet();  // NativeMask.cpp: a space is merged in this frame
	[[nodiscard]] json DescribeLighting();

	// Called by NativeMask.cpp's hook on BSLightingShader::SetupGeometry around the engine's own:
	// changes what the engine reads for this pass, and Restore puts everything back.
	struct LightingSwap
	{
		LightingSwap(RE::BSShader* a_shader, RE::BSRenderPass* a_pass);
		~LightingSwap();
		LightingSwap(const LightingSwap&) = delete;
		LightingSwap& operator=(const LightingSwap&) = delete;

		struct Saved;

	private:
		Saved* saved = nullptr;
	};
}
