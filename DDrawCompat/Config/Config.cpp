#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include <Common/Log.h>
#include <Common/Path.h>
#include <Config/Config.h>

namespace Config
{
	Logging logging = Logging::OFF;
	float alternatePixelCenter = 0.0f;
	VSync vSync = VSync::APP;
	unsigned vSyncInterval = 1;
	FpsLimiter fpsLimiter = FpsLimiter::OFF;
	unsigned fpsLimit = 60;
	bool fpsCounter = false;
	bool palettizedTextures = true;
	bool dialogColors = true;
	unsigned cpuAffinity = 1;

	// Defaults for the reported hardware: what a DirectX 7 era game expects to see, and small enough
	// to avoid the integer overflows many of them hit on modern systems (CPU speed in Hz overflowing
	// a signed 32-bit value above ~2.1 GHz, or memory sizes above 2 GB).
	unsigned cpuSpeed = 2000;
	unsigned videoMemory = 256;
	unsigned systemMemory = 1024;
}

namespace
{
	// Messages produced while reading the ini, logged later by Config::logSettings().
	std::vector<std::string> g_messages;

	struct Value
	{
		std::string name;
		bool hasParam;
		int param;
	};

	std::string toLower(std::string str)
	{
		std::transform(str.begin(), str.end(), str.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return str;
	}

	std::string trim(const std::string& str)
	{
		const char* ws = " \t\r\n";
		const auto begin = str.find_first_not_of(ws);
		if (std::string::npos == begin)
		{
			return std::string();
		}
		return str.substr(begin, str.find_last_not_of(ws) - begin + 1);
	}

	bool parseInt(const std::string& str, int& result)
	{
		if (str.empty())
		{
			return false;
		}
		try
		{
			size_t pos = 0;
			result = std::stoi(str, &pos);
			return pos == str.size();
		}
		catch (...)
		{
			return false;
		}
	}

	// Parses "name" or "name(param)", e.g. "on", "on(2)", "flipstart(60)", "custom(-50)".
	// A bare number is returned as name "" with a param.
	bool parseValue(const std::string& str, Value& value)
	{
		value = { std::string(), false, 0 };
		const auto open = str.find('(');
		if (std::string::npos == open)
		{
			if (parseInt(str, value.param))
			{
				value.hasParam = true;
				return true;
			}
			value.name = str;
			return !str.empty();
		}

		if (str.back() != ')')
		{
			return false;
		}
		value.name = trim(str.substr(0, open));
		value.hasParam = parseInt(trim(str.substr(open + 1, str.size() - open - 2)), value.param);
		return value.hasParam;
	}

	bool inRange(const Value& value, int min, int max)
	{
		return value.hasParam && value.param >= min && value.param <= max;
	}

	// "app" (or 0) leaves the value unchanged; a positive number overrides it.
	bool parseOverride(const Value& value, unsigned max, unsigned& result)
	{
		if ("app" == value.name && !value.hasParam)
		{
			result = 0;
			return true;
		}
		if (value.name.empty() && inRange(value, 0, static_cast<int>(max)))
		{
			result = value.param;
			return true;
		}
		return false;
	}

	bool parseOnOff(const Value& value, bool& result)
	{
		if (value.hasParam || ("on" != value.name && "off" != value.name))
		{
			return false;
		}
		result = "on" == value.name;
		return true;
	}

	// "app", "all" or a comma-separated list of 1-based CPU core numbers, e.g. "1" or "1,2".
	bool parseCpuAffinity(const std::string& str, unsigned& result)
	{
		if ("app" == str) { result = 0; return true; }
		if ("all" == str) { result = 0xFFFFFFFF; return true; }

		unsigned mask = 0;
		std::string::size_type start = 0;
		while (start <= str.size())
		{
			auto end = str.find(',', start);
			if (std::string::npos == end)
			{
				end = str.size();
			}
			int core = 0;
			if (!parseInt(trim(str.substr(start, end - start)), core) || core < 1 || core > 32)
			{
				return false;
			}
			mask |= 1U << (core - 1);
			start = end + 1;
		}
		result = mask;
		return 0 != mask;
	}

	bool applySetting(const std::string& key, const std::string& valueStr, const Value& value)
	{
		if ("logging" == key)
		{
			if (value.hasParam) { return false; }
			if ("off" == value.name) { Config::logging = Config::Logging::OFF; return true; }
			if ("on" == value.name) { Config::logging = Config::Logging::ON; return true; }
			if ("verbose" == value.name) { Config::logging = Config::Logging::VERBOSE; return true; }
			return false;
		}

		if ("fpscounter" == key)
		{
			return parseOnOff(value, Config::fpsCounter);
		}

		if ("palettizedtextures" == key)
		{
			return parseOnOff(value, Config::palettizedTextures);
		}

		if ("dialogcolors" == key)
		{
			return parseOnOff(value, Config::dialogColors);
		}

		if ("cpuaffinity" == key)
		{
			return parseCpuAffinity(valueStr, Config::cpuAffinity);
		}

		if ("alternatepixelcenter" == key)
		{
			if ("off" == value.name && !value.hasParam) { Config::alternatePixelCenter = 0.0f; return true; }
			if ("on" == value.name && !value.hasParam) { Config::alternatePixelCenter = -0.5f; return true; }
			if ("custom" == value.name && inRange(value, -100, 100))
			{
				Config::alternatePixelCenter = value.param / 100.0f;
				return true;
			}
			return false;
		}

		if ("vsync" == key)
		{
			if ("app" == value.name && !value.hasParam) { Config::vSync = Config::VSync::APP; return true; }
			if ("off" == value.name && !value.hasParam) { Config::vSync = Config::VSync::OFF; return true; }
			if ("on" == value.name && (!value.hasParam || inRange(value, 1, 16)))
			{
				Config::vSync = Config::VSync::ON;
				Config::vSyncInterval = value.hasParam ? value.param : 1;
				return true;
			}
			return false;
		}

		if ("fpslimiter" == key)
		{
			if ("off" == value.name && !value.hasParam) { Config::fpsLimiter = Config::FpsLimiter::OFF; return true; }
			if (("flipstart" == value.name || "flipend" == value.name) && (!value.hasParam || inRange(value, 10, 1000)))
			{
				Config::fpsLimiter = "flipstart" == value.name ? Config::FpsLimiter::FLIPSTART : Config::FpsLimiter::FLIPEND;
				Config::fpsLimit = value.hasParam ? value.param : 60;
				return true;
			}
			return false;
		}

		if ("cpuspeed" == key)
		{
			return parseOverride(value, 100000, Config::cpuSpeed);
		}

		if ("videomemory" == key)
		{
			return parseOverride(value, 4095, Config::videoMemory);
		}

		if ("systemmemory" == key)
		{
			return parseOverride(value, 4095, Config::systemMemory);
		}

		g_messages.push_back("Config: unknown setting \"" + key + '"');
		return true;
	}

	void loadFile(const std::filesystem::path& path)
	{
		std::ifstream file(path);
		if (!file)
		{
			return;
		}

		g_messages.push_back("Loading config file: " + path.u8string());
		std::string line;
		unsigned lineNumber = 0;
		while (std::getline(file, line))
		{
			++lineNumber;
			const auto comment = line.find_first_of("#;");
			if (std::string::npos != comment)
			{
				line.erase(comment);
			}

			line = trim(line);
			if (line.empty() || '[' == line.front())
			{
				continue;
			}

			const auto eq = line.find('=');
			if (std::string::npos == eq)
			{
				g_messages.push_back("Config: line " + std::to_string(lineNumber) + ": missing '='");
				continue;
			}

			const std::string key = toLower(trim(line.substr(0, eq)));
			const std::string valueStr = toLower(trim(line.substr(eq + 1)));
			Value value = {};
			if (!parseValue(valueStr, value) || !applySetting(key, valueStr, value))
			{
				g_messages.push_back("Config: line " + std::to_string(lineNumber) + ": invalid value \"" +
					valueStr + "\" for setting \"" + key + '"');
			}
		}
	}
}

namespace Config
{
	void load(const std::filesystem::path& dllPath, const std::filesystem::path& processPath)
	{
		const auto dllIni = Compat::replaceFilename(dllPath, "DDrawCompat.ini");
		const auto processIni = Compat::replaceFilename(processPath, "DDrawCompat.ini");

		loadFile(dllIni);
		if (!Compat::isEqual(dllIni, processIni))
		{
			loadFile(processIni);
		}

	}

	void logSettings()
	{
		for (const auto& message : g_messages)
		{
			Compat::Log() << message;
		}
		g_messages.clear();

		auto overrideStr = [](unsigned value, const char* unit)
		{
			return 0 == value ? std::string("app") : std::to_string(value) + ' ' + unit;
		};

		Compat::Log() << "Config: Logging = " << (Logging::VERBOSE == logging ? "verbose" : "on");
		Compat::Log() << "Config: AlternatePixelCenter = " << alternatePixelCenter;
		Compat::Log() << "Config: VSync = " << (VSync::APP == vSync ? "app" : VSync::OFF == vSync ? "off" : "on")
			<< (VSync::ON == vSync ? "(" + std::to_string(vSyncInterval) + ")" : std::string());
		Compat::Log() << "Config: FpsLimiter = "
			<< (FpsLimiter::OFF == fpsLimiter ? "off" : FpsLimiter::FLIPSTART == fpsLimiter ? "flipstart" : "flipend")
			<< (FpsLimiter::OFF != fpsLimiter ? "(" + std::to_string(fpsLimit) + ")" : std::string());
		Compat::Log() << "Config: FpsCounter = " << (fpsCounter ? "on" : "off");
		Compat::Log() << "Config: PalettizedTextures = " << (palettizedTextures ? "on" : "off");
		Compat::Log() << "Config: DialogColors = " << (dialogColors ? "on" : "off");
		Compat::Log() << "Config: CpuAffinity = " << (0 == cpuAffinity ? std::string("app")
			: 0xFFFFFFFF == cpuAffinity ? std::string("all") : "mask " + std::to_string(cpuAffinity));
		Compat::Log() << "Config: CpuSpeed = " << overrideStr(cpuSpeed, "MHz");
		Compat::Log() << "Config: VideoMemory = " << overrideStr(videoMemory, "MB");
		Compat::Log() << "Config: SystemMemory = " << overrideStr(systemMemory, "MB");
	}
}
