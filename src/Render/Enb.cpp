#include "Render/Enb.h"

namespace
{
	// From the ENBSeries SDK (enbseries.h, SDK 1000+).
	enum class ParameterType : std::uint32_t
	{
		kNone = 0,
		kFloat = 1,
		kInt = 2,
		kHex = 3,
		kBool = 4,
		kColor3 = 5,
		kColor4 = 6,
		kVector3 = 7,
	};

	struct Parameter
	{
		std::uint8_t  data[16];
		std::uint32_t size;
		ParameterType type;
	};

	using GetLong_t = long(__stdcall*)();
	using GetParameter_t = BOOL(__stdcall*)(char* a_file, char* a_category, char* a_key, Parameter* a_out);

	struct Sdk
	{
		HMODULE        module = nullptr;
		std::string    moduleName;
		long           sdkVersion = 0;
		long           version = 0;
		long           game = 0;
		GetParameter_t getParameter = nullptr;
	} sdk;

	std::optional<bool> BoolParameter(const char* a_file, const char* a_category, const char* a_key)
	{
		if (!sdk.getParameter) {
			return std::nullopt;
		}
		// The SDK takes non-const strings.
		std::string file(a_file), category(a_category), key(a_key);
		Parameter   value{};
		if (!sdk.getParameter(file.data(), category.data(), key.data(), &value) || value.type != ParameterType::kBool) {
			return std::nullopt;
		}
		return value.data[0] != 0;
	}
}

namespace Render::Enb
{
	void Detect()
	{
		// ENB is usually d3d11.dll; some setups load it as dxgi.dll or through a chain loader.
		for (const auto* name : { L"d3d11.dll", L"dxgi.dll", L"d3dcompiler_46e.dll" }) {
			auto* module = ::GetModuleHandleW(name);
			if (!module) {
				continue;
			}
			const auto sdkVersion = reinterpret_cast<GetLong_t>(::GetProcAddress(module, "ENBGetSDKVersion"));
			if (!sdkVersion) {
				continue;
			}
			sdk.module = module;
			sdk.moduleName = std::filesystem::path(name).string();
			sdk.sdkVersion = sdkVersion();
			if (const auto version = reinterpret_cast<GetLong_t>(::GetProcAddress(module, "ENBGetVersion"))) {
				sdk.version = version();
			}
			if (const auto game = reinterpret_cast<GetLong_t>(::GetProcAddress(module, "ENBGetGameIdentifier"))) {
				sdk.game = game();
			}
			sdk.getParameter = reinterpret_cast<GetParameter_t>(::GetProcAddress(module, "ENBGetParameter"));
			break;
		}
		if (sdk.module) {
			const auto effects = BoolParameter("enbseries.ini", "GLOBAL", "UseEffect");
			logger::info("render: ENB {} (SDK {}) found in {}, effects {}", sdk.version, sdk.sdkVersion, sdk.moduleName, effects ? (*effects ? "on" : "off") : "unknown");
		}
	}

	bool Present()
	{
		return sdk.module != nullptr;
	}

	json Describe()
	{
		if (!sdk.module) {
			return { { "present", false } };
		}
		const auto effects = BoolParameter("enbseries.ini", "GLOBAL", "UseEffect");
		return {
			{ "present", true },
			{ "module", sdk.moduleName },
			{ "version", sdk.version },
			{ "sdkVersion", sdk.sdkVersion },
			{ "gameIdentifier", std::format("{:X}", sdk.game) },
			{ "effects", effects ? json(*effects) : json(nullptr) },
		};
	}
}
