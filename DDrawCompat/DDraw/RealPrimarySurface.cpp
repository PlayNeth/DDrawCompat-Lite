#include <atomic>
#include <memory>
#include <vector>

#include <Common/CompatPtr.h>
#include <Common/Hook.h>
#include <Common/Time.h>
#include <Config/Config.h>
#include <D3dDdi/Device.h>
#include <D3dDdi/KernelModeThunks.h>
#include <DDraw/DirectDraw.h>
#include <DDraw/DirectDrawSurface.h>
#include <DDraw/FpsCounter.h>
#include <DDraw/IReleaseNotifier.h>
#include <DDraw/RealPrimarySurface.h>
#include <DDraw/ScopedThreadLock.h>
#include <DDraw/Surfaces/PrimarySurface.h>
#include <DDraw/Types.h>
#include <Gdi/Caret.h>
#include <Gdi/Gdi.h>
#include <Gdi/Palette.h>
#include <Gdi/VirtualScreen.h>
#include <Gdi/Window.h>
#include <Win32/DisplayMode.h>

namespace
{
	void onRelease();

	CompatWeakPtr<IDirectDrawSurface7> g_frontBuffer;
	CompatWeakPtr<IDirectDrawClipper> g_clipper;
	DDSURFACEDESC2 g_surfaceDesc = {};
	DDraw::IReleaseNotifier g_releaseNotifier(onRelease);

	bool g_isFullScreen = false;
	DDraw::Surface* g_lastFlipSurface = nullptr;

	bool g_isUpdatePending = false;
	bool g_waitingForPrimaryUnlock = false;
	std::atomic<long long> g_qpcLastUpdate = 0;
	UINT g_flipEndVsyncCount = 0;
	UINT g_presentEndVsyncCount = 0;

	CompatPtr<IDirectDrawSurface7> getBackBuffer();
	CompatPtr<IDirectDrawSurface7> getLastSurface();

	void bltToPrimaryChain(CompatRef<IDirectDrawSurface7> src)
	{
		if (!g_isFullScreen)
		{
			Gdi::Window::present(*g_frontBuffer, src, *g_clipper);
			return;
		}

		auto backBuffer(getBackBuffer());
		if (backBuffer)
		{
			backBuffer->Blt(backBuffer, nullptr, &src, nullptr, DDBLT_WAIT, nullptr);
		}
	}

	CompatPtr<IDirectDrawSurface7> getBackBuffer()
	{
		DDSCAPS2 caps = {};
		caps.dwCaps = DDSCAPS_BACKBUFFER;
		CompatPtr<IDirectDrawSurface7> backBuffer;
		if (g_frontBuffer)
		{
			g_frontBuffer->GetAttachedSurface(g_frontBuffer, &caps, &backBuffer.getRef());
		}
		return backBuffer;
	}

	CompatPtr<IDirectDrawSurface7> getLastSurface()
	{
		DDSCAPS2 caps = {};
		caps.dwCaps = DDSCAPS_FLIP;;
		CompatPtr<IDirectDrawSurface7> backBuffer(getBackBuffer());
		CompatPtr<IDirectDrawSurface7> lastSurface;
		if (backBuffer)
		{
			backBuffer->GetAttachedSurface(backBuffer, &caps, &lastSurface.getRef());
		}
		return lastSurface;
	}

	UINT getFlipInterval(DWORD flags)
	{
		switch (Config::vSync)
		{
		case Config::VSync::OFF:
			return 0;
		case Config::VSync::ON:
			return Config::vSyncInterval;
		default:
			break;
		}

		if (flags & DDFLIP_NOVSYNC)
		{
			return 0;
		}

		if (flags & (DDFLIP_INTERVAL2 | DDFLIP_INTERVAL3 | DDFLIP_INTERVAL4))
		{
			UINT flipInterval = (flags & (DDFLIP_INTERVAL2 | DDFLIP_INTERVAL3 | DDFLIP_INTERVAL4)) >> 24;
			if (flipInterval < 2 || flipInterval > 4)
			{
				flipInterval = 1;
			}
			return flipInterval;
		}

		return 1;
	}

	bool isFlipPending()
	{
		return static_cast<INT>(D3dDdi::KernelModeThunks::getVsyncCounter() - g_flipEndVsyncCount) < 0;
	}

	bool isPresentPending()
	{
		return static_cast<INT>(D3dDdi::KernelModeThunks::getVsyncCounter() - g_presentEndVsyncCount) < 0;
	}

	void onRelease()
	{
		LOG_FUNC("RealPrimarySurface::onRelease");

		DDraw::FpsCounter::releaseResources();
		g_frontBuffer = nullptr;
		g_clipper.release();
		g_isFullScreen = false;
		g_waitingForPrimaryUnlock = false;
		g_surfaceDesc = {};
	}

	void onRestore()
	{
		DDSURFACEDESC2 desc = {};
		desc.dwSize = sizeof(desc);
		g_frontBuffer->GetSurfaceDesc(g_frontBuffer, &desc);

		g_clipper.release();

		const bool isFlippable = 0 != (desc.ddsCaps.dwCaps & DDSCAPS_FLIP);
		if (!isFlippable)
		{
			CALL_ORIG_PROC(DirectDrawCreateClipper)(0, &g_clipper.getRef(), nullptr);
			g_frontBuffer->SetClipper(g_frontBuffer, g_clipper);
		}

		g_surfaceDesc = desc;
		g_isFullScreen = isFlippable;
		g_isUpdatePending = false;
		g_qpcLastUpdate = Time::queryPerformanceCounter() - Time::msToQpc(Config::delayedFlipModeTimeout);

		if (isFlippable)
		{
			g_frontBuffer->Flip(g_frontBuffer, getLastSurface(), DDFLIP_WAIT);
			g_flipEndVsyncCount = D3dDdi::KernelModeThunks::getVsyncCounter() + 1;
			g_presentEndVsyncCount = g_flipEndVsyncCount;
			D3dDdi::KernelModeThunks::waitForVsyncCounter(g_flipEndVsyncCount);
		}
	}

	struct GdiWindowEnumData
	{
		DWORD processId;
		HWND deviceWindow;
		Gdi::Region covered;
		Gdi::Region gdiWindows;
	};

	BOOL CALLBACK addGdiWindowRegion(HWND hwnd, LPARAM lParam)
	{
		auto& data = *reinterpret_cast<GdiWindowEnumData*>(lParam);
		DWORD processId = 0;
		GetWindowThreadProcessId(hwnd, &processId);
		if (processId != data.processId || !IsWindowVisible(hwnd) || IsIconic(hwnd))
		{
			return TRUE;
		}

		RECT rect = {};
		GetWindowRect(hwnd, &rect);
		if (hwnd == data.deviceWindow)
		{
			data.covered |= rect;
			return TRUE;
		}

		// Layered windows (menus, DDrawCompat's own presentation windows) are presented separately.
		if ((GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) ||
			Gdi::MENU_ATOM == GetClassLong(hwnd, GCW_ATOM))
		{
			return TRUE;
		}

		Gdi::Region rgn(rect);
		rgn -= data.covered;
		data.gdiWindows |= rgn;
		data.covered |= rect;
		return TRUE;
	}

	// Finds the visible parts of the game's other top-level windows (dialogs etc.) on the primary
	// surface. These are drawn by GDI, which uses the standard static colors of the system palette,
	// even if the game has overwritten those entries in its own palette.
	void updateGdiWindowRects(CompatWeakPtr<IDirectDrawSurface7> src, const RECT& monitorRect)
	{
		std::vector<RECT> rects;
		const bool isDialogColorsActive =
			Config::dialogColors && g_isFullScreen && 8 == g_surfaceDesc.ddpfPixelFormat.dwRGBBitCount;

		// Windows drawn with the default palette selected (most dialogs) are normally limited to the
		// 20 static colors. Since they are presented with the system palette, which holds the game's
		// colors in the other entries, let them use all of it (icons and bitmaps keep their colors).
		Gdi::VirtualScreen::setDefaultPaletteFull(isDialogColorsActive);

		// Listing the windows is only needed when the palettes differ, which most games never cause.
		if (isDialogColorsActive && src == DDraw::PrimarySurface::getGdiSurface() &&
			Gdi::Palette::isSystemPaletteDifferent())
		{
			GdiWindowEnumData data = { GetCurrentProcessId(), DDraw::PrimarySurface::getDeviceWindow() };
			EnumWindows(addGdiWindowRegion, reinterpret_cast<LPARAM>(&data));
			data.gdiWindows &= monitorRect;
			if (!data.gdiWindows.isEmpty())
			{
				data.gdiWindows.offset(-monitorRect.left, -monitorRect.top);
				const DWORD size = GetRegionData(data.gdiWindows, 0, nullptr);
				std::vector<BYTE> buffer(size);
				auto rgnData = reinterpret_cast<RGNDATA*>(buffer.data());
				if (size && GetRegionData(data.gdiWindows, size, rgnData))
				{
					auto first = reinterpret_cast<RECT*>(rgnData->Buffer);
					rects.assign(first, first + rgnData->rdh.nCount);
				}
			}
		}
		Gdi::Palette::setGdiWindowRects(std::move(rects));
	}

	void presentToPrimaryChain(CompatWeakPtr<IDirectDrawSurface7> src)
	{
		LOG_FUNC("RealPrimarySurface::presentToPrimaryChain", src);

		Gdi::VirtualScreen::update();

		if (!g_frontBuffer || !src || DDraw::RealPrimarySurface::isLost())
		{
			Gdi::Window::present(nullptr);
			return;
		}

		RECT monitorRect = D3dDdi::KernelModeThunks::getMonitorRect();
		Gdi::Region excludeRegion(monitorRect);
		Gdi::Window::present(excludeRegion);

		updateGdiWindowRects(src, monitorRect);
		D3dDdi::KernelModeThunks::setDcPaletteOverride(true);
		bltToPrimaryChain(*src);
		D3dDdi::KernelModeThunks::setDcPaletteOverride(false);

		if (g_isFullScreen)
		{
			auto backBuffer(getBackBuffer());
			if (backBuffer)
			{
				if (src == DDraw::PrimarySurface::getGdiSurface())
				{
					POINT offset = { -monitorRect.left, -monitorRect.top };
					Gdi::Window::presentLayered(*backBuffer, offset);
				}
				DDraw::FpsCounter::draw(*backBuffer, g_surfaceDesc.dwHeight);
			}
		}
	}

	void updateNow(CompatWeakPtr<IDirectDrawSurface7> src, UINT flipInterval)
	{
		DDraw::FpsCounter::onPresent();
		presentToPrimaryChain(src);
		g_isUpdatePending = false;
		g_waitingForPrimaryUnlock = false;

		if (g_isFullScreen)
		{
			g_frontBuffer->Flip(g_frontBuffer, getBackBuffer(), DDFLIP_WAIT);
		}
		g_presentEndVsyncCount = D3dDdi::KernelModeThunks::getVsyncCounter() + max(flipInterval, 1);
	}

	void updateNowIfNotBusy()
	{
		auto primary(DDraw::PrimarySurface::getPrimary());
		RECT emptyRect = {};
		HRESULT result = primary ? primary->BltFast(primary, 0, 0, primary, &emptyRect, DDBLTFAST_WAIT) : DD_OK;
		g_waitingForPrimaryUnlock = DDERR_SURFACEBUSY == result || DDERR_LOCKEDSURFACES == result;

		if (!g_waitingForPrimaryUnlock)
		{
			const auto msSinceLastUpdate = Time::qpcToMs(Time::queryPerformanceCounter() - g_qpcLastUpdate);
			updateNow(primary, msSinceLastUpdate > Config::delayedFlipModeTimeout ? 0 : 1);
		}
	}

	unsigned WINAPI updateThreadProc(LPVOID /*lpParameter*/)
	{
		bool skipWaitForVsync = false;

		while (true)
		{
			if (!skipWaitForVsync)
			{
				D3dDdi::KernelModeThunks::waitForVsync();
			}
			skipWaitForVsync = false;
			Gdi::Caret::blink();
			Sleep(1);

			DDraw::ScopedThreadLock lock;
			if (g_isUpdatePending && !isPresentPending())
			{
				auto qpcNow = Time::queryPerformanceCounter();
				auto qpcLastVsync = D3dDdi::KernelModeThunks::getQpcLastVsync();
				if (Time::qpcToMs(qpcNow - qpcLastVsync) < 1 ||
					Time::qpcToMs(qpcNow - g_qpcLastUpdate) < 1 && Time::qpcToMs(qpcNow - qpcLastVsync) <= 3)
				{
					skipWaitForVsync = true;
				}
				else
				{
					updateNowIfNotBusy();
				}
			}
		}

		return 0;
	}
}

namespace DDraw
{
	template <typename DirectDraw>
	HRESULT RealPrimarySurface::create(CompatRef<DirectDraw> dd)
	{
		DDraw::ScopedThreadLock lock;

		typename Types<DirectDraw>::TSurfaceDesc desc = {};
		desc.dwSize = sizeof(desc);
		desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
		desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP;
		desc.dwBackBufferCount = 2;

		CompatPtr<typename Types<DirectDraw>::TCreatedSurface> surface;
		HRESULT result = dd->CreateSurface(&dd, &desc, &surface.getRef(), nullptr);

		if (DDERR_NOEXCLUSIVEMODE == result)
		{
			desc.dwFlags = DDSD_CAPS;
			desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
			desc.dwBackBufferCount = 0;
			result = dd->CreateSurface(&dd, &desc, &surface.getRef(), nullptr);
		}

		if (FAILED(result))
		{
			Compat::Log() << "ERROR: Failed to create the real primary surface: " << Compat::hex(result);
			return result;
		}

		g_frontBuffer = CompatPtr<IDirectDrawSurface7>::from(surface.get()).detach();
		g_frontBuffer->SetPrivateData(g_frontBuffer, IID_IReleaseNotifier,
			&g_releaseNotifier, sizeof(&g_releaseNotifier), DDSPD_IUNKNOWNPOINTER);
		onRestore();

		return DD_OK;
	}

	template HRESULT RealPrimarySurface::create(CompatRef<IDirectDraw>);
	template HRESULT RealPrimarySurface::create(CompatRef<IDirectDraw2>);
	template HRESULT RealPrimarySurface::create(CompatRef<IDirectDraw4>);
	template HRESULT RealPrimarySurface::create(CompatRef<IDirectDraw7>);

	HRESULT RealPrimarySurface::flip(CompatPtr<IDirectDrawSurface7> surfaceTargetOverride, DWORD flags)
	{
		DDraw::FpsCounter::onFlip();
		const DWORD flipInterval = getFlipInterval(flags);
		if (0 == flipInterval)
		{
			g_isUpdatePending = true;
			return DD_OK;
		}

		const auto msSinceLastUpdate = Time::qpcToMs(Time::queryPerformanceCounter() - g_qpcLastUpdate);
		const bool isFlipDelayed = msSinceLastUpdate >= 0 && msSinceLastUpdate <= Config::delayedFlipModeTimeout;
		if (isFlipDelayed)
		{
			if (!isPresentPending())
			{
				CompatPtr<IDirectDrawSurface7> prevPrimarySurface(
					surfaceTargetOverride ? surfaceTargetOverride : PrimarySurface::getLastSurface());
				updateNow(prevPrimarySurface, 0);
			}
			g_isUpdatePending = true;
		}
		else
		{
			updateNow(PrimarySurface::getPrimary(), flipInterval);
		}
		g_flipEndVsyncCount = D3dDdi::KernelModeThunks::getVsyncCounter() + flipInterval;
		g_presentEndVsyncCount = g_flipEndVsyncCount;

		if (0 != flipInterval)
		{
			g_lastFlipSurface = Surface::getSurface(
				surfaceTargetOverride ? *surfaceTargetOverride : *PrimarySurface::getLastSurface());
		}
		else
		{
			g_lastFlipSurface = nullptr;
		}
		return DD_OK;
	}

	void RealPrimarySurface::flush()
	{
		DDraw::ScopedThreadLock lock;
		if (g_isUpdatePending && !isPresentPending())
		{
			updateNowIfNotBusy();
		}
	}

	HRESULT RealPrimarySurface::getGammaRamp(DDGAMMARAMP* rampData)
	{
		DDraw::ScopedThreadLock lock;
		auto gammaControl(CompatPtr<IDirectDrawGammaControl>::from(g_frontBuffer.get()));
		if (!gammaControl)
		{
			return DDERR_INVALIDPARAMS;
		}

		return gammaControl->GetGammaRamp(gammaControl, 0, rampData);
	}

	CompatWeakPtr<IDirectDrawSurface7> RealPrimarySurface::getSurface()
	{
		return g_frontBuffer;
	}

	void RealPrimarySurface::init()
	{
		Dll::createThread(&updateThreadProc, nullptr, THREAD_PRIORITY_TIME_CRITICAL);
	}

	bool RealPrimarySurface::isFullScreen()
	{
		return g_isFullScreen;
	}

	bool RealPrimarySurface::isLost()
	{
		DDraw::ScopedThreadLock lock;
		return g_frontBuffer && DDERR_SURFACELOST == g_frontBuffer->IsLost(g_frontBuffer);
	}

	void RealPrimarySurface::release()
	{
		DDraw::ScopedThreadLock lock;
		g_frontBuffer.release();
	}

	HRESULT RealPrimarySurface::restore()
	{
		DDraw::ScopedThreadLock lock;
		HRESULT result = g_frontBuffer->Restore(g_frontBuffer);
		if (SUCCEEDED(result))
		{
			onRestore();
		}
		return result;
	}

	void RealPrimarySurface::scheduleUpdate()
	{
		g_qpcLastUpdate = Time::queryPerformanceCounter();
		g_isUpdatePending = true;
	}

	HRESULT RealPrimarySurface::setGammaRamp(DDGAMMARAMP* rampData)
	{
		DDraw::ScopedThreadLock lock;
		auto gammaControl(CompatPtr<IDirectDrawGammaControl>::from(g_frontBuffer.get()));
		if (!gammaControl)
		{
			return DDERR_INVALIDPARAMS;
		}

		return gammaControl->SetGammaRamp(gammaControl, 0, rampData);
	}

	void RealPrimarySurface::update()
	{
		DDraw::ScopedThreadLock lock;
		g_qpcLastUpdate = Time::queryPerformanceCounter();
		g_isUpdatePending = true;
		if (g_waitingForPrimaryUnlock)
		{
			updateNowIfNotBusy();
		}
	}

	bool RealPrimarySurface::waitForFlip(Surface* surface, bool wait)
	{
		auto primary(DDraw::PrimarySurface::getPrimary());
		if (!surface || !primary ||
			surface != g_lastFlipSurface &&
			surface != Surface::getSurface(*DDraw::PrimarySurface::getPrimary()))
		{
			return true;
		}

		if (!wait)
		{
			return !isFlipPending();
		}

		D3dDdi::KernelModeThunks::waitForVsyncCounter(g_flipEndVsyncCount);
		return true;
	}

	void RealPrimarySurface::waitForFpsLimit()
	{
		if (Config::FpsLimiter::OFF == Config::fpsLimiter || 0 == Config::fpsLimit || 0 == Time::g_qpcFrequency)
		{
			return;
		}

		static long long g_qpcPrevWaitEnd = Time::queryPerformanceCounter() - Time::g_qpcFrequency;
		long long qpcNow = Time::queryPerformanceCounter();
		const long long qpcWaitEnd = g_qpcPrevWaitEnd + Time::g_qpcFrequency / Config::fpsLimit;
		if (qpcNow - qpcWaitEnd >= 0)
		{
			// Already late (or first frame): restart the schedule from now instead of trying to catch up.
			g_qpcPrevWaitEnd = qpcNow;
			return;
		}
		g_qpcPrevWaitEnd = qpcWaitEnd;

		const HANDLE thread = GetCurrentThread();
		const int prevPriority = GetThreadPriority(thread);
		SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);

		// Coarse wait with 1 ms sleeps (timeBeginPeriod(1) is set in DllMain), keeping the
		// delayed presentation going since the DirectDraw lock is held by the caller.
		while (Time::qpcToMs(qpcWaitEnd - qpcNow) > 1)
		{
			Sleep(1);
			flush();
			qpcNow = Time::queryPerformanceCounter();
		}

		// Spin for the remainder for accurate frame pacing.
		while (qpcWaitEnd - qpcNow > 0)
		{
			qpcNow = Time::queryPerformanceCounter();
		}

		if (THREAD_PRIORITY_ERROR_RETURN != prevPriority)
		{
			SetThreadPriority(thread, prevPriority);
		}
	}
}
