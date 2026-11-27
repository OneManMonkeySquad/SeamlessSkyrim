// SeamlessSkyrim's part of the shaders: what the plugin sends, and the rules that read it. The same
// file in every patched shader base. It is kept in SeamlessSkyrim's repository beside the struct
// it must match (src/Render/ShaderData.h), so a new field changes the plugin and this file, and
// no base's DLL.
//
// Included by Common/SharedData.hlsli inside namespace SharedData, after its cbuffer, whose last
// field is `float4 SeamlessData[64];`. The base supplies:
// - Permutation::ExtraFlags::SeamlessShift: the lowest of five bits of ExtraShaderDescriptor it
//   leaves free (the shape's tag, below);
// - SEAMLESS_VIEW_PROJ_INVERSE and SEAMLESS_CAMERA_POS_ADJUST, defined before the include, where
//   the frame buffer keeps them per eye.

#ifndef SEAMLESS_VIEW_PROJ_INVERSE
#	define SEAMLESS_VIEW_PROJ_INVERSE FrameBuffer::CameraViewProjInverse
#endif
#ifndef SEAMLESS_CAMERA_POS_ADJUST
#	define SEAMLESS_CAMERA_POS_ADJUST FrameBuffer::CameraPosAdjust
#endif

// SeamlessData, as Render::ShaderData. Up to four rooms are merged in behind their doors
// (render-merge), room 0 the one the player can walk into. Functions, not macros: the shader
// compiler's preprocessor doesn't expand a name written after SharedData::.
float4 SeamlessDoorPlane(uint room) { return SeamlessData[room]; }       // xyz: unit normal through the door into the merged space, w: 1 in use
float4 SeamlessDoorCenter(uint room) { return SeamlessData[4 + room]; }  // xyz: doorway centre in world coordinates, w: half width
float4 SeamlessDoorRight(uint room) { return SeamlessData[8 + room]; }   // xyz: unit horizontal axis of the doorway, w: half height
// x: 1 while any room is merged in, y: room 0 is a city gate masking the world this far beyond it,
// z: 1 with the camera in room 0, w: 1 with the sun's shadows on for the merged street only, 2 when
// it also takes the sky's IBL (SeamlessStreetIBL)
float4 SeamlessParams() { return SeamlessData[12]; }
// Lighting of each merged space: its directional light and directional ambient, in world space.
float4 SeamlessRemoteDirColor(uint room) { return SeamlessData[13 + room]; }      // w: 1 when set
float4 SeamlessRemoteDirDirection(uint room) { return SeamlessData[17 + room]; }  // xyz: towards the light, w: scale of the space's lights and ambient
float4 SeamlessRemoteAmbient(uint row) { return SeamlessData[21 + row]; }         // three rows of the 3x4 directional ambient matrix per room
float4 SeamlessPortalClip() { return SeamlessData[33]; }                         // portal pass: world plane (xyz unit normal, w offset), kept where dot(P, n) >= w; 0 = off
// Distance fog of the merged space (the street merged into an interior): as the engine's FogParam
// (x near / (far - near), y 1 / (far - near), z power, w most it covers) and its near and far
// colours; w of the near colour: 1 when set.
float4 SeamlessRemoteFogParam() { return SeamlessData[34]; }
float4 SeamlessRemoteFogNear() { return SeamlessData[35]; }
float4 SeamlessRemoteFogFar() { return SeamlessData[36]; }
// Rooms merged into the street, seen from it: the street's light coming in through the doorway, as
// the opening's radiance (rgb; 0 with the camera close to the doorway, where the room looks as it
// does from inside).
float4 SeamlessRoomDaylight(uint room) { return SeamlessData[37 + room]; }
// The street's fog, for the air between the camera and a merged room's doorway; as
// SeamlessRemoteFog*.
float4 SeamlessFrontFogParam() { return SeamlessData[41]; }
float4 SeamlessFrontFogNear() { return SeamlessData[42]; }
float4 SeamlessFrontFogFar() { return SeamlessData[43]; }
// Room 0 merged into the interior the player is in: its light coming out through its doorway into
// the player's space, as the opening's radiance (rgb). The merged room takes the player's space's
// in through SeamlessRoomDaylight(0) the same way.
float4 SeamlessHostDaylight() { return SeamlessData[44]; }

// The tag of the shape being drawn (SeamlessPortal_ShapeTag). Macros, not functions: only the
// shaders that mask include Common/Permutation.hlsli.
#define SEAMLESS_TAG (Permutation::ExtraShaderDescriptor >> Permutation::ExtraFlags::SeamlessShift)
#define SeamlessShapeRemote ((SEAMLESS_TAG & 1) != 0)      // a shape of a space merged in behind a door
#define SeamlessShapeUnmasked ((SEAMLESS_TAG & 2) != 0)    // drawn on both sides of the doorway (the player)
#define SeamlessShapeRoom ((SEAMLESS_TAG >> 2) & 3)        // which merged room a remote shape is in
#define SeamlessShapeLightView ((SEAMLESS_TAG & 16) != 0)  // drawn into a shadow map, from a light: never masked

// Camera-relative position of a pixel of the main view from its SV_Position. The depth
// prepass (Utility.hlsl) and the lighting pass must mask exactly the same pixels, or the
// doorway's edge z-fights; both derive the position this way, from identical inputs.
float3 SeamlessPixelPosition(float4 svPosition)
{
	float2 uv = svPosition.xy * BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
	float4 position = mul(SEAMLESS_VIEW_PROJ_INVERSE, float4(2 * float2(uv.x, 1 - uv.y) - 1, svPosition.z, 1));
	return position.xyz / position.w;
}

// A merged space's directional ambient for a normal (like the per-geometry DirectionalAmbient).
float3 SeamlessRemoteAmbientFor(float3 normal, uint room)
{
	return mul(float3x4(SeamlessRemoteAmbient(room * 3), SeamlessRemoteAmbient(room * 3 + 1), SeamlessRemoteAmbient(room * 3 + 2)), float4(normal, 1.0));
}

// The street merged into an interior is lit with the sky's IBL, as outdoors, though IBL is off
// for the interior itself (ImageBasedLighting::GetStreetDiffuseIBL).
bool SeamlessStreetIBL()
{
	return SeamlessParams().w > 1.5;
}

// SeamlessSkyrim: spaces drawn in one pass, each merged room moved so that its door lies on the
// world's. A line of sight from the camera passes into another space at the first doorway it
// goes through (from the camera's side, to a point beyond the door plane), and each pixel is
// drawn only where it is in the space its line of sight has reached: a room only through its
// own doorway and in front of any other doorway on the way, the world wherever no doorway
// leads elsewhere. mergedSpace: the shape is a merged room's, room which one. Returns true if
// the pixel must not be drawn.
bool SeamlessDoorClips(float3 positionCameraRelative, bool mergedSpace, uint room = 0)
{
	if (SeamlessParams().x <= 0)
		return false;
	// "Beyond the door" needs a margin: the two spaces' door frames stand in the same place, and
	// for pixels within rounding of the plane the side flips from frame to frame, which shows
	// as z-fighting between the two frames. Up to the margin a pixel stays with the camera's space.
	const float margin = 5.0;
	// Which space the camera is in comes from the plugin, not from its side of the door plane:
	// behind the plane is also the ground next to the house, where the camera is still in the
	// world (seen from there the world disappeared and the room stood in its place). A line of
	// sight only leads into the other space when it passes the doorway from the camera's side.
	// Only room 0 can hold the camera; from there only its own doorway leads anywhere (out), so
	// the other rooms are not seen at all.
	bool cameraMerged = SeamlessParams().z > 0.5;
	int reached = -1;  // the doorway the line of sight passes first, -1 none
	float reachedAt = 2.0;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		if (SeamlessDoorPlane(i).w <= 0 || (cameraMerged && i > 0))
			continue;
		float3 normal = SeamlessDoorPlane(i).xyz;
		// Made camera-relative here, with the camera of the pass being drawn: the CPU side has no
		// reliable "current camera" when the shared data is uploaded.
		float3 center = SeamlessDoorCenter(i).xyz - SEAMLESS_CAMERA_POS_ADJUST.xyz;
		float sidePixel = dot(positionCameraRelative - center, normal);  // > 0: beyond the door, the merged space's side
		float sideCamera = dot(-center, normal);
		bool camera = cameraMerged && i == 0;
		// Room 0 as a city gate (SeamlessParams().y): the other worldspace is the portal pass's, and
		// through the gate the world is its own again beyond where its far gate stands (the two
		// share coordinates). In between is the world's own side of the gatehouse, which the
		// other one has in its place: Solitude's showed a wall where the sky was.
		bool passed = i == 0 && SeamlessParams().y > 0 && sidePixel > SeamlessParams().y;
		if (!passed && (camera ? (sideCamera > 0 && sidePixel < -margin) : (sideCamera < 0 && sidePixel > margin))) {
			float along = sideCamera / (sideCamera - sidePixel);
			float3 hit = positionCameraRelative * along - center;
			if (along < reachedAt && abs(dot(hit, SeamlessDoorRight(i).xyz)) < SeamlessDoorCenter(i).w && abs(hit.z) < SeamlessDoorRight(i).w) {
				reached = int(i);
				reachedAt = along;
			}
		}
	}
	// The space the line of sight is in at the pixel: the camera's own unless it passed a doorway.
	// Through room 0's doorway with the camera inside, that is the world.
	bool seenMerged = reached < 0 ? cameraMerged : !(cameraMerged && reached == 0);
	if (mergedSpace != seenMerged)
		return true;
	return mergedSpace && (reached < 0 ? 0u : uint(reached)) != room;
}

// SeamlessSkyrim: the merged room a line of sight from a camera outside every room passes into
// first, and how far along the line to the pixel its doorway is (0..1); -1: none. The rule of
// SeamlessDoorClips for that camera.
int SeamlessDoorPassed(float3 positionCameraRelative, out float along)
{
	along = 1;
	int reached = -1;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		if (SeamlessDoorPlane(i).w <= 0)
			continue;
		float3 normal = SeamlessDoorPlane(i).xyz;
		float3 center = SeamlessDoorCenter(i).xyz - SEAMLESS_CAMERA_POS_ADJUST.xyz;
		float sidePixel = dot(positionCameraRelative - center, normal);
		float sideCamera = dot(-center, normal);
		if (sideCamera < 0 && sidePixel > 0) {
			float t = sideCamera / (sideCamera - sidePixel);
			float3 hit = positionCameraRelative * t - center;
			if (t < along && abs(dot(hit, SeamlessDoorRight(i).xyz)) < SeamlessDoorCenter(i).w && abs(hit.z) < SeamlessDoorRight(i).w) {
				reached = int(i);
				along = t;
			}
		}
	}
	return reached;
}

float SeamlessFogAmount(float4 param, float distance)
{
	return min(param.w, pow(saturate(distance * param.y - param.x), param.z));
}

// SeamlessSkyrim: a pixel of a merged room seen from the street through its doorway is fogged
// by the street's air up to the doorway and by the room's beyond it. Without the street's, a
// doorway across the market stood out sharp and bright from the haze on the walls around it.
// False for any other pixel. color, amount: the fog's colour and FogParam.w as the engine's,
// before Color::Fog and Color::FogAlpha.
bool SeamlessRoomFog(float3 positionCameraRelative, out float3 color, out float amount)
{
	color = 0;
	amount = 0;
	float along;
	if (SeamlessFrontFogNear().w <= 0 || SeamlessParams().z > 0.5 || SeamlessDoorPassed(positionCameraRelative, along) < 0)
		return false;
	float distance = length(positionCameraRelative);
	float front = SeamlessFogAmount(SeamlessFrontFogParam(), distance * along);
	float3 frontColor = lerp(SeamlessFrontFogNear().xyz, SeamlessFrontFogFar().xyz, front);
	float behind = SeamlessRemoteFogNear().w > 0 ? SeamlessFogAmount(SeamlessRemoteFogParam(), distance * (1 - along)) : 0;
	float3 behindColor = lerp(SeamlessRemoteFogNear().xyz, SeamlessRemoteFogFar().xyz, behind);
	// The room's fog, then the street's over it, as one.
	amount = 1 - (1 - behind) * (1 - front);
	color = amount > 0 ? (behindColor * behind * (1 - front) + frontColor * front) / amount : frontColor;
	return true;
}

// SeamlessSkyrim: how much of a point's view of its surroundings a merged room's doorway takes
// up, cosine-weighted (the whole hemisphere is 1): the form factor of a rectangle, exact for a
// diffuse surface, without anything in the room standing in the way.
float SeamlessDoorwayView(float3 positionCameraRelative, float3 normal, uint room)
{
	float3 center = SeamlessDoorCenter(room).xyz - SEAMLESS_CAMERA_POS_ADJUST.xyz - positionCameraRelative;
	float3 right = SeamlessDoorRight(room).xyz * SeamlessDoorCenter(room).w;
	float3 up = float3(0, 0, SeamlessDoorRight(room).w);
	float3 corners[4] = { normalize(center - right - up), normalize(center + right - up), normalize(center + right + up), normalize(center - right + up) };
	float3 sum = 0;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		float3 edge = cross(corners[i], corners[(i + 1) & 3]);
		sum += edge * (acos(clamp(dot(corners[i], corners[(i + 1) & 3]), -1, 1)) / max(length(edge), 1e-6));
	}
	// Wound either way round depending on the side it is seen from.
	if (dot(sum, center) < 0)
		sum = -sum;
	return saturate(dot(sum, normal) / (2 * 3.14159265));
}

// SeamlessSkyrim portal pass: its camera stands behind the far door, inside that building's
// shell. Pushing the near plane up to the door only clipped what lay between them when looking
// straight through the doorway; looking past the door's edge from close by, the back of the
// building's doorway (a closed door, from behind) showed in the portal. The door's plane clips
// exactly at any angle.
bool SeamlessPortalClips(float3 positionCameraRelative)
{
	if (!any(SeamlessPortalClip().xyz))
		return false;
	return dot(positionCameraRelative + SEAMLESS_CAMERA_POS_ADJUST.xyz, SeamlessPortalClip().xyz) < SeamlessPortalClip().w;
}

// Whether the pixel of the shape with this tag (SEAMLESS_TAG) must not be drawn: cut at the far
// door in a portal pass, or masked at a merged doorway. mainView: the position comes from
// SV_Position, exactly as in the depth prepass, or the two passes z-fight at a doorway's edge;
// other views (reflections, cubemaps) have no prepass to agree with and take worldPosition.
bool SeamlessMasks(float4 svPosition, float3 worldPosition, bool mainView, uint tag)
{
	if ((tag & 16) || (SeamlessParams().x <= 0 && !any(SeamlessPortalClip().xyz)))
		return false;
	float3 position = mainView ? SeamlessPixelPosition(svPosition) : worldPosition;
	if (SeamlessPortalClips(position))
		return true;
	return SeamlessParams().x > 0 && !(tag & 2) && SeamlessDoorClips(position, tag & 1, (tag >> 2) & 3);
}

// The light that comes through a merged doorway from its other side, to add to a pixel's ambient:
// rgb the opening's radiance, w how much of the pixel's view the doorway takes. A room merged into
// the street takes the street's (lit by its own alone it was warm and even to its threshold against
// the daylight round the door), and room 0 lights the player's space in turn, so that two rooms
// joined by an open door look joined from either side. remoteLit: a merged shape with its own light.
float4 SeamlessDoorwayLight(float3 positionCameraRelative, float3 normal, bool remoteLit, bool remoteShape, uint room)
{
	if (remoteLit && any(SeamlessRoomDaylight(room).rgb > 0))
		return float4(SeamlessRoomDaylight(room).rgb, SeamlessDoorwayView(positionCameraRelative, normal, room));
	if (!remoteShape && SeamlessDoorPlane(0).w > 0 && any(SeamlessHostDaylight().rgb > 0))
		return float4(SeamlessHostDaylight().rgb, SeamlessDoorwayView(positionCameraRelative, normal, 0));
	return 0;
}

// The fog of a merged space's pixel, as SeamlessRoomFog gives it; false: fogged as the frame.
// The street merged into an interior is fogged as outdoors: the interior's fog, a few thousand
// units deep and dark, turned the distant mountains black. throughDoorway: also a room seen from
// the street (SeamlessRoomFog).
bool SeamlessRemoteFog(float3 positionCameraRelative, bool throughDoorway, out float3 color, out float amount)
{
	if (throughDoorway && SeamlessRoomFog(positionCameraRelative, color, amount))
		return true;
	color = 0;
	amount = 0;
	if (SeamlessRemoteFogNear().w <= 0)
		return false;
	const float4 fog = SeamlessRemoteFogParam();
	amount = min(fog.w, exp2(fog.z * log2(saturate(length(positionCameraRelative) * fog.y - fog.x))));
	color = lerp(SeamlessRemoteFogNear().xyz, SeamlessRemoteFogFar().xyz, amount);
	return true;
}
