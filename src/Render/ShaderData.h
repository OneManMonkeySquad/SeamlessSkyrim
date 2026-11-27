#pragma once

// What the patched Community Shaders' shaders read of SeamlessSkyrim. Sent whole
// (SeamlessPortal_SetShaderData) and copied into their shared data as it is, so a new field changes
// this struct and shaders/Common/Seamless.hlsli, which must match it, and no shader base's DLL.
namespace Render
{
	struct Float4
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
	};

	struct ShaderData
	{
		Float4 doorPlane[4];           // xyz: unit normal through the door into the merged space, w: 1 in use
		Float4 doorCenter[4];          // xyz: doorway centre in world coordinates, w: half width
		Float4 doorRight[4];           // xyz: unit horizontal axis of the doorway, w: half height
		Float4 params;                 // x: any room merged, y: room 0 a city gate masking this far, z: camera in room 0, w: 1 street sun shadows, 2 also its IBL
		Float4 remoteDirColor[4];      // w: 1 when the merged space has its own lighting
		Float4 remoteDirDirection[4];  // xyz: towards the light, w: scale of the space's lights and ambient
		Float4 remoteAmbient[12];      // per room, rows of the 3x4 directional ambient matrix
		Float4 portalClip;             // portal pass: world plane, kept where dot(P, n) >= w; 0 = off
		Float4 remoteFogParam;         // as the engine's FogParam
		Float4 remoteFogNear;          // w: 1 when set
		Float4 remoteFogFar;
		Float4 roomDaylight[4];        // the street's light through each room's doorway (radiance)
		Float4 frontFogParam;
		Float4 frontFogNear;           // w: 1 when set
		Float4 frontFogFar;
		Float4 hostDaylight;           // room 0's light into the player's space through its doorway (radiance)
		Float4 unused[19];             // the forks' array is 64 long: room to grow without rebuilding them
	};
	inline constexpr int kShaderDataFloat4s = 64;
	static_assert(sizeof(ShaderData) == kShaderDataFloat4s * sizeof(Float4));

	// The tag of a shape being drawn (SeamlessPortal_ShapeTag), five bits.
	namespace ShapeTag
	{
		inline constexpr std::uint32_t kRemote = 1 << 0;     // a shape of a space merged in behind a door
		inline constexpr std::uint32_t kUnmasked = 1 << 1;   // drawn on both sides of the doorway (the player)
		inline constexpr std::uint32_t kRoomShift = 2;       // two bits: which merged room a remote shape is in
		inline constexpr std::uint32_t kLightView = 1 << 4;  // drawn into a shadow map, from a light: never masked
	}
}
