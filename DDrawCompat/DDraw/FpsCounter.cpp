#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <Common/CompatPtr.h>
#include <Common/Hook.h>
#include <Common/Log.h>
#include <Common/Time.h>
#include <Config/Config.h>
#include <D3dDdi/KernelModeThunks.h>
#include <DDraw/FpsCounter.h>

namespace
{
	const long long STATS_WINDOW_MS = 10000;
	const long long TEXT_UPDATE_MS = 500;
	const long long FLIP_TIMEOUT_MS = 1000;

	// Timestamps of recent frames (QPC), used as a ring buffer.
	std::array<long long, 8192> g_frames = {};
	unsigned g_frameIndex = 0;
	unsigned g_frameCount = 0;

	long long g_qpcLastFlip = 0;
	long long g_qpcLastTextUpdate = 0;
	char g_text[96] = {};

	long long g_qpcSessionStart = 0;
	unsigned long long g_sessionFrames = 0;
	std::vector<float> g_sessionFrameTimesMs;

	HFONT g_font = nullptr;
	LONG g_fontHeight = 0;

	// The text is drawn once per update into a small system memory surface, which is then blitted to
	// the back buffer every frame. Drawing with GetDC on the back buffer itself would force a full
	// readback and upload of the back buffer on every frame.
	CompatPtr<IDirectDrawSurface7> g_textSurface;
	void* g_textSurfaceDd = nullptr;
	void* g_lastBackBuffer = nullptr;
	SIZE g_textSurfaceSize = {};
	bool g_isTextSurfaceDirty = true;

	void addFrame(long long qpcNow)
	{
		if (0 == g_qpcSessionStart)
		{
			g_qpcSessionStart = qpcNow;
		}
		else if (g_frameCount > 0)
		{
			const auto prev = g_frames[(g_frameIndex + g_frames.size() - 1) % g_frames.size()];
			// Kept for the whole-session lows in the verbose log; gaps of a second or more are
			// loading screens or pauses rather than gameplay, so they are left out.
			const float frameTimeMs = static_cast<float>((qpcNow - prev) * 1000.0 / Time::g_qpcFrequency);
			if (frameTimeMs < 1000 && g_sessionFrameTimesMs.size() < 1000000 && Compat::Log::isVerbose())
			{
				g_sessionFrameTimesMs.push_back(frameTimeMs);
			}
		}

		g_frames[g_frameIndex] = qpcNow;
		g_frameIndex = (g_frameIndex + 1) % g_frames.size();
		g_frameCount = std::min<unsigned>(g_frameCount + 1, g_frames.size());
		++g_sessionFrames;
	}

	// Average FPS of the slowest `fraction` of frames (e.g. 0.01 for the "1% low").
	double getLowFps(std::vector<double>& frameTimesMs, double fraction)
	{
		const std::size_t count = std::max<std::size_t>(1, static_cast<std::size_t>(frameTimesMs.size() * fraction));
		std::partial_sort(frameTimesMs.begin(), frameTimesMs.begin() + count, frameTimesMs.end(), std::greater<double>());
		double sum = 0;
		for (std::size_t i = 0; i < count; ++i)
		{
			sum += frameTimesMs[i];
		}
		return sum > 0 ? 1000.0 * count / sum : 0;
	}

	void updateText(long long qpcNow)
	{
		const long long windowStart = qpcNow - Time::msToQpc(STATS_WINDOW_MS);
		const long long secondStart = qpcNow - Time::msToQpc(1000);

		std::vector<double> frameTimesMs;
		frameTimesMs.reserve(g_frameCount);
		unsigned framesLastSecond = 0;
		long long first = 0;
		long long last = 0;

		for (unsigned i = 0; i < g_frameCount; ++i)
		{
			const auto qpc = g_frames[(g_frameIndex + g_frames.size() - g_frameCount + i) % g_frames.size()];
			if (qpc < windowStart)
			{
				continue;
			}
			if (0 != last)
			{
				frameTimesMs.push_back((qpc - last) * 1000.0 / Time::g_qpcFrequency);
			}
			else
			{
				first = qpc;
			}
			last = qpc;
			if (qpc >= secondStart)
			{
				++framesLastSecond;
			}
		}

		if (frameTimesMs.empty())
		{
			sprintf_s(g_text, "FPS %u", framesLastSecond);
			return;
		}

		const double average = frameTimesMs.size() * 1000.0 * Time::g_qpcFrequency / (last - first) / 1000.0;
		const double low1 = getLowFps(frameTimesMs, 0.01);
		const double low01 = getLowFps(frameTimesMs, 0.001);
		sprintf_s(g_text, "FPS %u | avg %.1f | 1%% low %.1f | 0.1%% low %.1f",
			framesLastSecond, average, low1, low01);
	}

	HFONT getFont(LONG height)
	{
		if (!g_font || height != g_fontHeight)
		{
			if (g_font)
			{
				DeleteObject(g_font);
			}
			g_font = CreateFontA(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
			g_fontHeight = height;
		}
		return g_font;
	}
}

namespace DDraw
{
	namespace FpsCounter
	{
		void draw(CompatRef<IDirectDrawSurface7> surface, DWORD surfaceHeight)
		{
			if (!Config::fpsCounter)
			{
				return;
			}

			const long long qpcNow = Time::queryPerformanceCounter();
			if (Time::qpcToMs(qpcNow - g_qpcLastTextUpdate) >= TEXT_UPDATE_MS)
			{
				g_qpcLastTextUpdate = qpcNow;
				updateText(qpcNow);
				g_isTextSurfaceDirty = true;
			}
			if (!g_text[0])
			{
				return;
			}

			// The DirectDraw object only needs to be looked up when the text surface must be (re)created.
			if (g_textSurface && (&surface != g_lastBackBuffer || FAILED(g_textSurface->IsLost(g_textSurface))))
			{
				releaseResources();
			}
			g_lastBackBuffer = &surface;

			CompatPtr<IDirectDraw7> dd;
			if (!g_textSurface)
			{
				CompatPtr<IUnknown> ddUnknown;
				surface->GetDDInterface(&surface, reinterpret_cast<void**>(&ddUnknown.getRef()));
				if (!ddUnknown)
				{
					return;
				}
				IDirectDraw7* ddRaw = nullptr;
				ddUnknown->QueryInterface(ddUnknown, IID_IDirectDraw7, reinterpret_cast<void**>(&ddRaw));
				dd.reset(ddRaw);
				if (!dd)
				{
					return;
				}
			}

			const LONG height = std::max<LONG>(12, static_cast<LONG>(surfaceHeight) / 45);
			const int margin = height / 3;
			const int length = static_cast<int>(strlen(g_text));

			if (!g_textSurface || g_isTextSurfaceDirty)
			{
				HDC measureDc = CreateCompatibleDC(nullptr);
				auto prevMeasureFont = SelectObject(measureDc, getFont(height));
				SIZE textSize = {};
				GetTextExtentPoint32A(measureDc, g_text, length, &textSize);
				SelectObject(measureDc, prevMeasureFont);
				DeleteDC(measureDc);

				// Round the width up so small changes in the text don't recreate the surface every update.
				const SIZE size = { (textSize.cx + 2 * margin + 63) / 64 * 64, textSize.cy + margin };
				if (g_textSurface && (size.cx != g_textSurfaceSize.cx || size.cy != g_textSurfaceSize.cy))
				{
					// Recreate it with the DirectDraw object it was created with (same back buffer).
					dd = CompatPtr<IDirectDraw7>::from(static_cast<IDirectDraw7*>(g_textSurfaceDd));
					g_textSurface.release();
					g_textSurfaceDd = nullptr;
				}

				if (!g_textSurface)
				{
					if (!dd)
					{
						return;
					}
					DDSURFACEDESC2 desc = {};
					desc.dwSize = sizeof(desc);
					desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
					desc.dwWidth = size.cx;
					desc.dwHeight = size.cy;
					desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
					desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
					desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
					desc.ddpfPixelFormat.dwRGBBitCount = 32;
					desc.ddpfPixelFormat.dwRBitMask = 0xFF0000;
					desc.ddpfPixelFormat.dwGBitMask = 0x00FF00;
					desc.ddpfPixelFormat.dwBBitMask = 0x0000FF;
					if (FAILED(dd->CreateSurface(dd, &desc, &g_textSurface.getRef(), nullptr)))
					{
						return;
					}
					g_textSurfaceDd = dd.get();
					g_textSurfaceSize = size;
				}

				HDC dc = nullptr;
				g_textSurface->GetDC(g_textSurface, &dc);
				if (!dc)
				{
					return;
				}
				auto prevFont = SelectObject(dc, getFont(height));
				SetTextColor(dc, RGB(255, 255, 0));
				SetBkColor(dc, RGB(0, 0, 0));
				RECT rect = { 0, 0, size.cx, size.cy };
				CALL_ORIG_FUNC(ExtTextOutA)(dc, margin, margin / 2, ETO_OPAQUE, &rect, g_text, length, nullptr);
				SelectObject(dc, prevFont);
				g_textSurface->ReleaseDC(g_textSurface, dc);
				g_isTextSurfaceDirty = false;
			}

			RECT dstRect = { margin, margin, margin + g_textSurfaceSize.cx, margin + g_textSurfaceSize.cy };
			surface->Blt(&surface, &dstRect, g_textSurface, nullptr, DDBLT_WAIT, nullptr);
		}

		void releaseResources()
		{
			g_textSurface.release();
			g_textSurfaceDd = nullptr;
			g_lastBackBuffer = nullptr;
			g_isTextSurfaceDirty = true;
		}

		void logSummary()
		{
			if (0 == g_sessionFrames || 0 == g_qpcSessionStart || 0 == Time::g_qpcFrequency)
			{
				return;
			}
			const long long last = g_frames[(g_frameIndex + g_frames.size() - 1) % g_frames.size()];
			const double seconds = (last - g_qpcSessionStart) * 1.0 / Time::g_qpcFrequency;
			if (seconds <= 0)
			{
				return;
			}

			std::vector<double> frameTimesMs(g_sessionFrameTimesMs.begin(), g_sessionFrameTimesMs.end());
			const double low1 = frameTimesMs.empty() ? 0 : getLowFps(frameTimesMs, 0.01);
			const double low01 = frameTimesMs.empty() ? 0 : getLowFps(frameTimesMs, 0.001);

			char text[200] = {};
			sprintf_s(text, "Frame rate summary: %llu frames in %.1f s, average %.1f FPS, 1%% low %.1f, 0.1%% low %.1f"
				" (lows exclude pauses of 1 s or more)",
				g_sessionFrames, seconds, (g_sessionFrames - 1) / seconds, low1, low01);
			Compat::Log() << text;

			// A few long frames are loading hitches; many of them mean real stutter during play.
			unsigned over50 = 0;
			unsigned over100 = 0;
			unsigned over250 = 0;
			for (const float frameTimeMs : g_sessionFrameTimesMs)
			{
				over50 += frameTimeMs > 50;
				over100 += frameTimeMs > 100;
				over250 += frameTimeMs > 250;
			}
			sprintf_s(text, "Long frames: %u over 50 ms, %u over 100 ms, %u over 250 ms (out of %u)",
				over50, over100, over250, static_cast<unsigned>(g_sessionFrameTimesMs.size()));
			Compat::Log() << text;
		}

		void onFlip()
		{
			if (!Config::fpsCounter && !Compat::Log::isVerbose())
			{
				return;
			}
			g_qpcLastFlip = Time::queryPerformanceCounter();
			addFrame(g_qpcLastFlip);
		}

		void onPresent()
		{
			if (!Config::fpsCounter && !Compat::Log::isVerbose())
			{
				return;
			}
			const long long qpcNow = Time::queryPerformanceCounter();
			if (Time::qpcToMs(qpcNow - g_qpcLastFlip) > FLIP_TIMEOUT_MS)
			{
				addFrame(qpcNow);
			}
		}
	}
}
