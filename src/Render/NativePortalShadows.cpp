#include "Render/Native.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <d3dcompiler.h>

// The sun's shadows in the portal pass (a city seen through its gate) with the game's own shaders.
//
// The game's lighting shader doesn't look the sun's shadow maps up itself: it reads a screen-space
// shadow mask (PS t14, .x the sun) that the engine builds for the main view from its depth and the
// shadow maps, before the lighting pass. The portal pass is a view of its own, so it gets a mask of
// its own: from the portal pass's depth (its own depth prepass, PortalRender) and the shadow maps
// drawn for the portal camera (RenderPortalShadows), by a small pass of ours, written like the
// engine's (Utility.hlsl, RENDER_SHADOWMASK: the cascade picked by view depth, the second blended
// in where they overlap, faded out towards the end of the last). The engine's own mask step is
// wired to the main view's camera, targets and state.
namespace
{
	struct Constants
	{
		float viewProjInverse[16];  // camera-relative clip to camera-relative world (the engine's)
		float posAdjust[4];         // camera-relative to absolute
		float forward[4];           // view direction, for the view depth the cascades are split by
		float shadowProj[2][16];    // absolute world to the cascade's map: uv in xy, depth in z
		float splits[4];            // end of cascade 0, end of cascade 1, start of cascade 1, bias
		float size[4];              // the picture's part of the target (pixels)
	};

	struct Pipeline
	{
		ID3D11VertexShader*      vertexShader = nullptr;
		ID3D11PixelShader*       pixelShader = nullptr;
		ID3D11Buffer*            constants = nullptr;
		ID3D11DepthStencilState* depthState = nullptr;
		ID3D11RasterizerState*   rasterState = nullptr;
		ID3D11BlendState*        blendState = nullptr;
		ID3D11SamplerState*      compareSampler = nullptr;
		ID3D11Texture2D*         mask = nullptr;
		ID3D11RenderTargetView*  maskTarget = nullptr;
		ID3D11ShaderResourceView* maskView = nullptr;
		UINT                     width = 0;
		UINT                     height = 0;
	} pipeline;

	std::atomic<ID3D11ShaderResourceView*> activeMask = nullptr;

	constexpr char kSource[] = R"(
		cbuffer Mask : register(b0)
		{
			row_major float4x4 ViewProjInverse;  // as the engine keeps it: v * M
			float4 PosAdjust;
			float4 Forward;
			row_major float4x4 ShadowProj[2];
			float4 Splits;
			float4 Size;
		};
		Texture2D<float> Depth : register(t0);
		Texture2DArray<float> ShadowMaps : register(t1);
		SamplerComparisonState Compare : register(s0);

		float4 VS(uint id : SV_VertexID) : SV_Position
		{
			float2 uv = float2((id << 1) & 2, id & 2);
			return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
		}

		// A 3x3 grid of bilinear comparisons a texel apart (a 4x4 tent): the engine's own mask
		// blurs its edges further (rotated Poisson taps, then its temporal resolve).
		float Sample(float3 ls, uint cascade, float2 texel)
		{
			float compare = ls.z - Splits.w;
			float sum = 0;
			[unroll] for (int y = -1; y <= 1; ++y) {
				[unroll] for (int x = -1; x <= 1; ++x) {
					float weight = (x == 0 ? 2 : 1) * (y == 0 ? 2 : 1);
					sum += weight * ShadowMaps.SampleCmpLevelZero(Compare, float3(ls.xy + float2(x, y) * texel, cascade), compare);
				}
			}
			return sum / 16;
		}

		// The position is moved off the surface along its normal by about a texel of the cascade
		// before it is looked up: compared where it lies, a surface the sun grazes (the gatehouse's
		// walls) shadowed itself in bands, as the map's texels are wider than the surface is flat.
		float Cascade(float3 world, float3 normal, uint cascade)
		{
			uint w, h, slices;
			ShadowMaps.GetDimensions(w, h, slices);
			float2 texel = 1.0 / float2(w, h);
			float  uvPerUnit = length(float3(ShadowProj[cascade][0][0], ShadowProj[cascade][1][0], ShadowProj[cascade][2][0]));
			float  texelWorld = texel.x / max(uvPerUnit, 1e-8);
			float3 ls = mul(float4(world + normal * texelWorld * 1.5, 1), ShadowProj[cascade]).xyz;
			if (any(ls.xy < 0) || any(ls.xy > 1)) {
				return 1;
			}
			return Sample(ls, cascade, texel);
		}

		float3 Relative(float2 pixel, float depth)
		{
			float2 ndc = float2(pixel.x / Size.x * 2 - 1, 1 - pixel.y / Size.y * 2);
			float4 relative = mul(float4(ndc, depth, 1), ViewProjInverse);
			return relative.xyz / relative.w;
		}

		// Of the two neighbours along an axis, the one on the same surface (the nearer depth): the
		// other may be across an edge.
		float3 Along(float2 pixel, float depth, float3 here, float2 step)
		{
			float  before = Depth.Load(int3(pixel - step, 0));
			float  after = Depth.Load(int3(pixel + step, 0));
			return abs(after - depth) < abs(before - depth) ? Relative(pixel + step, after) - here : here - Relative(pixel - step, before);
		}

		float4 PS(float4 position : SV_Position) : SV_Target
		{
			float depth = Depth.Load(int3(position.xy, 0));
			if (depth >= 1) {
				return 1;  // the sky
			}
			float3 relative = Relative(position.xy, depth);
			float  viewDepth = dot(relative, Forward.xyz);
			if (viewDepth >= Splits.y) {
				return 1;
			}
			float3 world = relative + PosAdjust.xyz;
			// The surface's normal from the depth around, facing the camera.
			float3 normal = normalize(cross(Along(position.xy, depth, relative, float2(0, 1)), Along(position.xy, depth, relative, float2(1, 0))));
			if (dot(normal, relative) > 0) {
				normal = -normal;
			}
			float shadow;
			if (viewDepth < Splits.z) {
				shadow = Cascade(world, normal, 0);
			} else if (viewDepth >= Splits.x) {
				shadow = Cascade(world, normal, 1);
			} else {
				float blend = smoothstep(0, 1, (viewDepth - Splits.z) / max(Splits.x - Splits.z, 1e-3));
				shadow = lerp(Cascade(world, normal, 0), Cascade(world, normal, 1), blend);
			}
			float fade = saturate(viewDepth / Splits.y);
			shadow = lerp(1, shadow, 1 - pow(fade * fade, 8));
			return float4(shadow, 1, 1, 1);
		}
	)";

	void Compile(const char* a_entry, const char* a_profile, ID3DBlob** a_out)
	{
		ID3DBlob*     errors = nullptr;
		const HRESULT result = ::D3DCompile(kSource, sizeof(kSource) - 1, "SeamlessPortalShadowMask", nullptr, nullptr, a_entry, a_profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
		if (FAILED(result)) {
			logger::critical("native backend: portal shadow mask {} failed: {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
		}
		SEAMLESS_ASSERT(SUCCEEDED(result));
		if (errors) {
			errors->Release();
		}
	}

	void EnsurePipeline(ID3D11Device* a_device)
	{
		if (pipeline.vertexShader) {
			return;
		}
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		Compile("VS", "vs_5_0", &vs);
		Compile("PS", "ps_5_0", &ps);
		D3D11_BUFFER_DESC        buffer{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_DEPTH_STENCIL_DESC depth{};
		D3D11_RASTERIZER_DESC    raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		D3D11_BLEND_DESC blend{};
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		D3D11_SAMPLER_DESC compare{};
		compare.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		compare.AddressU = compare.AddressV = compare.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		compare.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		compare.MaxLOD = D3D11_FLOAT32_MAX;
		const bool made = SUCCEEDED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &pipeline.vertexShader)) &&
		                  SUCCEEDED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pipeline.pixelShader)) &&
		                  SUCCEEDED(a_device->CreateBuffer(&buffer, nullptr, &pipeline.constants)) && SUCCEEDED(a_device->CreateDepthStencilState(&depth, &pipeline.depthState)) &&
		                  SUCCEEDED(a_device->CreateRasterizerState(&raster, &pipeline.rasterState)) && SUCCEEDED(a_device->CreateBlendState(&blend, &pipeline.blendState)) &&
		                  SUCCEEDED(a_device->CreateSamplerState(&compare, &pipeline.compareSampler));
		SEAMLESS_ASSERT(made);
		vs->Release();
		ps->Release();
	}

	void EnsureMask(ID3D11Device* a_device, UINT a_width, UINT a_height)
	{
		if (pipeline.mask && pipeline.width == a_width && pipeline.height == a_height) {
			return;
		}
		for (IUnknown* object : std::initializer_list<IUnknown*>{ pipeline.maskView, pipeline.maskTarget, pipeline.mask }) {
			if (object) {
				object->Release();
			}
		}
		D3D11_TEXTURE2D_DESC desc{ a_width, a_height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
		const bool           made = SUCCEEDED(a_device->CreateTexture2D(&desc, nullptr, &pipeline.mask)) &&
		                  SUCCEEDED(a_device->CreateRenderTargetView(pipeline.mask, nullptr, &pipeline.maskTarget)) &&
		                  SUCCEEDED(a_device->CreateShaderResourceView(pipeline.mask, nullptr, &pipeline.maskView));
		SEAMLESS_ASSERT(made);
		pipeline.width = a_width;
		pipeline.height = a_height;
	}
}

namespace Render::Native
{
	void BuildPortalShadowMask(const PortalShadowInputs& a_in)
	{
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context);
		ID3D11Device* device = nullptr;
		context->GetDevice(&device);
		EnsurePipeline(device);
		EnsureMask(device, a_in.targetWidth, a_in.targetHeight);
		device->Release();

		Constants c{};
		{
			const auto       matrix = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&a_in.view->viewProjMat));
			const auto       inverse = DirectX::XMMatrixInverse(nullptr, matrix);
			DirectX::XMFLOAT4X4 stored;
			DirectX::XMStoreFloat4x4(&stored, inverse);
			std::memcpy(c.viewProjInverse, &stored, sizeof(c.viewProjInverse));
		}
		c.posAdjust[0] = a_in.posAdjust.x;
		c.posAdjust[1] = a_in.posAdjust.y;
		c.posAdjust[2] = a_in.posAdjust.z;
		std::memcpy(c.forward, &a_in.view->viewForward, sizeof(float) * 3);
		auto&      lightData = a_in.light->GetRuntimeData();
		const auto cascades = std::min<std::uint32_t>(lightData.shadowmapDescriptors.size(), 2);
		for (std::uint32_t i = 0; i < cascades; ++i) {
			std::memcpy(c.shadowProj[i], &lightData.shadowmapDescriptors[i].lightTransform, sizeof(float) * 16);
		}
		const auto& dir = a_in.light->GetShadowDirectionalLightRuntimeData();
		c.splits[0] = dir.endSplitDistances[0];
		c.splits[1] = cascades > 1 ? dir.endSplitDistances[1] : dir.endSplitDistances[0];
		c.splits[2] = cascades > 1 ? dir.startSplitDistances[1] : dir.endSplitDistances[0];
		c.splits[3] = a_in.bias;
		c.size[0] = a_in.pictureWidth;
		c.size[1] = a_in.pictureHeight;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		SEAMLESS_ASSERT(SUCCEEDED(context->Map(pipeline.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)));
		std::memcpy(mapped.pData, &c, sizeof(c));
		context->Unmap(pipeline.constants, 0);

		// Everything touched here goes back as it was: the engine tracks its own bindings.
		ID3D11RenderTargetView*   savedTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*   savedDepthView = nullptr;
		ID3D11DepthStencilState*  savedDepthState = nullptr;
		UINT                      savedStencilRef = 0;
		ID3D11BlendState*         savedBlend = nullptr;
		FLOAT                     savedBlendFactor[4]{};
		UINT                      savedSampleMask = 0;
		ID3D11RasterizerState*    savedRaster = nullptr;
		D3D11_VIEWPORT            savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT                      savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ID3D11VertexShader*       savedVS = nullptr;
		ID3D11PixelShader*        savedPS = nullptr;
		ID3D11GeometryShader*     savedGS = nullptr;
		ID3D11InputLayout*        savedLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY  savedTopology{};
		ID3D11Buffer*             savedConstants = nullptr;
		ID3D11ShaderResourceView* savedViews[2]{};
		ID3D11SamplerState*       savedSampler = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedTargets, &savedDepthView);
		context->OMGetDepthStencilState(&savedDepthState, &savedStencilRef);
		context->OMGetBlendState(&savedBlend, savedBlendFactor, &savedSampleMask);
		context->RSGetState(&savedRaster);
		context->RSGetViewports(&savedViewportCount, savedViewports);
		context->VSGetShader(&savedVS, nullptr, nullptr);
		context->PSGetShader(&savedPS, nullptr, nullptr);
		context->GSGetShader(&savedGS, nullptr, nullptr);
		context->IAGetInputLayout(&savedLayout);
		context->IAGetPrimitiveTopology(&savedTopology);
		context->PSGetConstantBuffers(0, 1, &savedConstants);
		context->PSGetShaderResources(0, 2, savedViews);
		context->PSGetSamplers(0, 1, &savedSampler);

		// The depth is read here, so it can't stay bound as the target.
		const D3D11_VIEWPORT viewport{ 0, 0, a_in.pictureWidth, a_in.pictureHeight, 0, 1 };
		context->OMSetRenderTargets(1, &pipeline.maskTarget, nullptr);
		constexpr float white[4]{ 1, 1, 1, 1 };
		context->ClearRenderTargetView(pipeline.maskTarget, white);
		context->OMSetDepthStencilState(pipeline.depthState, 0);
		context->OMSetBlendState(pipeline.blendState, nullptr, 0xFFFFFFFF);
		context->RSSetState(pipeline.rasterState);
		context->RSSetViewports(1, &viewport);
		context->VSSetShader(pipeline.vertexShader, nullptr, 0);
		context->PSSetShader(pipeline.pixelShader, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->PSSetConstantBuffers(0, 1, &pipeline.constants);
		ID3D11ShaderResourceView* views[2]{ a_in.depth, a_in.shadowMaps };
		context->PSSetShaderResources(0, 2, views);
		context->PSSetSamplers(0, 1, &pipeline.compareSampler);
		context->Draw(3, 0);

		ID3D11ShaderResourceView* none[2]{};
		context->PSSetShaderResources(0, 2, none);
		context->PSSetShaderResources(0, 2, savedViews);
		context->PSSetSamplers(0, 1, &savedSampler);
		context->PSSetConstantBuffers(0, 1, &savedConstants);
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedTargets, savedDepthView);
		context->OMSetDepthStencilState(savedDepthState, savedStencilRef);
		context->OMSetBlendState(savedBlend, savedBlendFactor, savedSampleMask);
		context->RSSetState(savedRaster);
		context->RSSetViewports(savedViewportCount, savedViewports);
		context->VSSetShader(savedVS, nullptr, 0);
		context->PSSetShader(savedPS, nullptr, 0);
		context->GSSetShader(savedGS, nullptr, 0);
		context->IASetInputLayout(savedLayout);
		context->IASetPrimitiveTopology(savedTopology);
		for (auto* target : savedTargets) {
			if (target) {
				target->Release();
			}
		}
		for (IUnknown* object : std::initializer_list<IUnknown*>{ savedDepthView, savedDepthState, savedBlend, savedRaster, savedVS, savedPS, savedGS, savedLayout, savedConstants,
				 savedViews[0], savedViews[1], savedSampler }) {
			if (object) {
				object->Release();
			}
		}
		activeMask = pipeline.maskView;
	}

	void EndPortalShadowMask()
	{
		activeMask = nullptr;
	}

	ID3D11ShaderResourceView* PortalShadowMask()
	{
		return activeMask;
	}
}
