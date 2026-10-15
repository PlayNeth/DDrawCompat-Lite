#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

#include <Common/Log.h>
#include <Common/Path.h>
#include <Config/Config.h>

namespace Config
{
	float alternatePixelCenter = 0.0f;
	VSync vSync = VSync::APP;
	unsigned vSyncInterval = 1;
	FpsLimiter fpsLimiter = FpsLimiter::OFF;
	unsigned fpsLimit = 60;
	unsigned cpuSpeed = 0;
	unsigned videoMemory = 0;
	unsigned systemMemory = 0;
}

namespace
{
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

	bool applySetting(const std::string& key, const Value& value)
	{
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

		Compat::Log() << "Config: unknown setting \"" << key << '"';
		return true;
	}

	void loadFile(const std::filesystem::path& path)
	{
		std::ifstream file(path);
		if (!file)
		{
			return;
		}

		Compat::Log() << "Loading config file: " << path.u8string();
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
				Compat::Log() << "Config: line " << lineNumber << ": missing '='";
				continue;
			}

			const std::string key = toLower(trim(line.substr(0, eq)));
			const std::string valueStr = toLower(trim(line.substr(eq + 1)));
			Value value = {};
			if (!parseValue(valueStr, value) || !applySetting(key, value))
			{
				Compat::Log() << "Config: line " << lineNumber << ": invalid value \"" << valueStr
					<< "\" for setting \"" << key << '"';
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

		Compat::Log() << "Config: AlternatePixelCenter = " << alternatePixelCenter;
		Compat::Log() << "Config: VSync = " << (VSync::APP == vSync ? "app" : VSync::OFF == vSync ? "off" : "on")
			<< (VSync::ON == vSync ? "(" + std::to_string(vSyncInterval) + ")" : std::string());
		Compat::Log() << "Config: FpsLimiter = "
			<< (FpsLimiter::OFF == fpsLimiter ? "off" : FpsLimiter::FLIPSTART == fpsLimiter ? "flipstart" : "flipend")
			<< (FpsLimiter::OFF != fpsLimiter ? "(" + std::to_string(fpsLimit) + ")" : std::string());
		Compat::Log() << "Config: CpuSpeed = " << cpuSpeed << " MHz (0 = app)";
		Compat::Log() << "Config: VideoMemory = " << videoMemory << " MB (0 = app)";
		Compat::Log() << "Config: SystemMemory = " << systemMemory << " MB (0 = app)";
	}
}
