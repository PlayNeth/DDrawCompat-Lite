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

	enum class VSync { APP, OFF, ON };
	enum class FpsLimiter { OFF, FLIPSTART, FLIPEND };

	extern float alternatePixelCenter;   // 0 = off; "on" = -0.5; custom(N) = N / 100
	extern VSync vSync;
	extern unsigned vSyncInterval;       // 1..16, used when vSync == ON
	extern FpsLimiter fpsLimiter;
	extern unsigned fpsLimit;            // max FPS, used when fpsLimiter != OFF
	extern unsigned cpuSpeed;            // reported CPU speed in MHz, 0 = app (unchanged)
	extern unsigned videoMemory;         // reported video memory in MB, 0 = app (unchanged)
	extern unsigned systemMemory;        // reported physical memory in MB, 0 = app (unchanged)

	void load(const std::filesystem::path& dllPath, const std::filesystem::path& processPath);
}
