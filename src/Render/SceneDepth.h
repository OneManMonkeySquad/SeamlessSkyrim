#pragma once

namespace Render
{
	// The scene's depth runs from 1 near to 0 far (Bottled Shaders' Reverse Z-Buffer) rather than
	// 0 near to 1 far. a_viewProj: the main view's camera-relative view-projection (row vectors), the
	// one the scene was drawn with. With the eye at the origin its last row is the projection's:
	// a positive constant added to clip z makes z/w fall with distance. The plugin's own shaders
	// that read a depth (a doorway's picture, a stand-in's capture) read it in that convention.
	// Depth tests need nothing: Bottled Shaders turns the states bound with its depth targets round.
	inline bool SceneDepthReversed(const float* a_viewProj)
	{
		return a_viewProj[14] > 0.0f;
	}
}
