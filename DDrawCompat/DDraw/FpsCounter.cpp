#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

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
	double g_worstFrameMs = 0;

	HFONT g_font = nullptr;
	LONG g_fontHeight = 0;

	void addFrame(long long qpcNow)
	{
		if (0 == g_qpcSessionStart)
		{
			g_qpcSessionStart = qpcNow;
		}
		else if (g_frameCount > 0)
		{
			const auto prev = g_frames[(g_frameIndex + g_frames.size() - 1) % g_frames.size()];
			g_worstFrameMs = std::max(g_worstFrameMs, (qpcNow - prev) * 1000.0 / Time::g_qpcFrequency);
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
			}
			if (!g_text[0])
			{
				return;
			}

			HDC dc = nullptr;
			const UINT D3DDDIFMT_UNKNOWN = 0;
			const UINT D3DDDIFMT_X8R8G8B8 = 22;
			D3dDdi::KernelModeThunks::setDcFormatOverride(D3DDDIFMT_X8R8G8B8);
			surface->GetDC(&surface, &dc);
			D3dDdi::KernelModeThunks::setDcFormatOverride(D3DDDIFMT_UNKNOWN);
			if (!dc)
			{
				return;
			}

			const LONG height = std::max<LONG>(12, static_cast<LONG>(surfaceHeight) / 45);
			auto prevFont = SelectObject(dc, getFont(height));
			SetTextColor(dc, RGB(255, 255, 0));
			SetBkColor(dc, RGB(0, 0, 0));

			const int length = static_cast<int>(strlen(g_text));
			SIZE size = {};
			GetTextExtentPoint32A(dc, g_text, length, &size);
			const int margin = height / 3;
			RECT rect = { margin, margin, margin + size.cx + 2 * margin, margin + size.cy + margin };
			CALL_ORIG_FUNC(ExtTextOutA)(dc, rect.left + margin, rect.top + margin / 2, ETO_OPAQUE, &rect,
				g_text, length, nullptr);

			SelectObject(dc, prevFont);
			surface->ReleaseDC(&surface, dc);
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

			char text[160] = {};
			sprintf_s(text, "Frame rate summary: %llu frames in %.1f s, average %.1f FPS, slowest frame %.1f ms",
				g_sessionFrames, seconds, (g_sessionFrames - 1) / seconds, g_worstFrameMs);
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
