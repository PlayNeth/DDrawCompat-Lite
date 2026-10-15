#pragma once

#include <vector>

#include <Windows.h>

namespace Gdi
{
	namespace Palette
	{
		PALETTEENTRY* getDefaultPalette();
		std::vector<PALETTEENTRY> getHardwarePalette();
		std::vector<PALETTEENTRY> getSystemPalette();
		std::vector<RECT> getGdiWindowRects();
		void installHooks();
		void setGdiWindowRects(std::vector<RECT> rects);
		void setHardwarePalette(PALETTEENTRY* entries);
	}
}
