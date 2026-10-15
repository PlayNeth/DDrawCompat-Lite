#pragma once

#include <ddraw.h>

#include <Common/CompatRef.h>

namespace DDraw
{
	// Minimal frame rate overlay (FpsCounter = on): current FPS, average, 1% and 0.1% lows.
	// Frames are counted at the game's flips; games that never flip are measured by screen updates.
	namespace FpsCounter
	{
		void draw(CompatRef<IDirectDrawSurface7> surface, DWORD surfaceHeight);
		void logSummary();
		void onFlip();
		void onPresent();
		void releaseResources();
	}
}
