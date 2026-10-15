#pragma once

#include <filesystem>

namespace Config
{
	const unsigned delayedFlipModeTimeout = 200;
	const unsigned evictionTimeout = 200;
	const unsigned maxPaletteUpdatesPerMs = 5;
	const unsigned maxUserModeDisplayDrivers = 3;
	const unsigned threadSwitchCycleTime = 3 * 1000 * 1000;

	// User settings read from DDrawCompat.ini (ddrawcompat-lite).
	// Key names and value syntax follow later DDrawCompat versions where an equivalent exists.

	enum class Logging { OFF, ON, VERBOSE };
	enum class VSync { APP, OFF, ON };
	enum class FpsLimiter { OFF, FLIPSTART, FLIPEND };

	extern Logging logging;
	extern float alternatePixelCenter;   // 0 = off; "on" = -0.5; custom(N) = N / 100
	extern VSync vSync;
	extern unsigned vSyncInterval;       // 1..16, used when vSync == ON
	extern FpsLimiter fpsLimiter;
	extern unsigned fpsLimit;            // max FPS, used when fpsLimiter != OFF
	extern bool fpsCounter;
	extern bool palettizedTextures;
	extern unsigned cpuAffinity;         // CPU core bit mask, 0 = app (unchanged)
	extern unsigned cpuSpeed;            // reported CPU speed in MHz, 0 = app (unchanged)
	extern unsigned videoMemory;         // reported video memory in MB, 0 = app (unchanged)
	extern unsigned systemMemory;        // reported physical memory in MB, 0 = app (unchanged)

	// Reads the ini files. Nothing is logged here, because the log file is only opened
	// once the Logging setting is known; call logSettings() after that.
	void load(const std::filesystem::path& dllPath, const std::filesystem::path& processPath);
	void logSettings();
}
