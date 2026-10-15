#include <map>
#include <sstream>
#include <string>
#include <type_traits>

#include <Common/CompatPtr.h>
#include <Common/CompatVtable.h>
#include <Common/Log.h>
#include <Config/Config.h>
#include <D3dDdi/KernelModeThunks.h>
#include <DDraw/DirectDraw.h>
#include <DDraw/RealPrimarySurface.h>
#include <DDraw/ScopedThreadLock.h>
#include <DDraw/Surfaces/PrimarySurface.h>
#include <DDraw/Visitors/DirectDrawVtblVisitor.h>

namespace
{
	void logComInstantiation()
	{
		LOG_ONCE("COM instantiation of DirectDraw detected");
	}

	std::map<std::string, unsigned> g_surfaceCounts;

	template <typename TSurfaceDesc>
	void logSurfaceCreation(const TSurfaceDesc& desc, HRESULT result)
	{
		if ((desc.dwFlags & DDSD_WIDTH) && !(desc.dwFlags & DDSD_HEIGHT))
		{
			return; // execute/vertex buffers (also created internally while installing hooks), not surfaces
		}

		std::string kind;
		const DWORD caps = desc.ddsCaps.dwCaps;
		if (caps & DDSCAPS_PRIMARYSURFACE) { kind = "primary"; }
		else if (caps & DDSCAPS_TEXTURE) { kind = "texture"; }
		else if (caps & DDSCAPS_ZBUFFER) { kind = "z-buffer"; }
		else if (caps & DDSCAPS_3DDEVICE) { kind = "render target"; }
		else if (caps & DDSCAPS_OFFSCREENPLAIN) { kind = "off-screen"; }
		else { kind = "surface"; }
		if (caps & DDSCAPS_SYSTEMMEMORY) { kind += " (system memory)"; }
		else if (caps & DDSCAPS_VIDEOMEMORY) { kind += " (video memory)"; }
		if (caps & DDSCAPS_MIPMAP) { kind += ", mipmapped"; }

		std::ostringstream oss;
		oss << kind;
		if (desc.dwFlags & DDSD_WIDTH) { oss << ", " << desc.dwWidth << 'x' << desc.dwHeight; }
		if (desc.dwFlags & DDSD_BACKBUFFERCOUNT) { oss << ", " << desc.dwBackBufferCount << " back buffer(s)"; }
		if (desc.dwFlags & DDSD_PIXELFORMAT)
		{
			const auto& pf = desc.ddpfPixelFormat;
			if (pf.dwFlags & DDPF_PALETTEINDEXED8) { oss << ", 8-bit palettized"; }
			else if (pf.dwFlags & DDPF_FOURCC) { oss << ", FourCC 0x" << std::hex << pf.dwFourCC << std::dec; }
			else { oss << ", " << pf.dwRGBBitCount << "-bit" << ((pf.dwFlags & DDPF_ALPHAPIXELS) ? " with alpha" : ""); }
		}
		if (FAILED(result))
		{
			oss << ", failed: 0x" << std::hex << static_cast<DWORD>(result);
		}

		// Games create thousands of identical surfaces (textures); log each kind once and count the rest.
		const std::string description = oss.str();
		if (1 == ++g_surfaceCounts[description])
		{
			Compat::Log() << "CreateSurface: " << description;
		}
	}

	template <typename TDirectDraw, typename TSurfaceDesc, typename TSurface>
	HRESULT STDMETHODCALLTYPE CreateSurface(
		TDirectDraw* This, TSurfaceDesc* lpDDSurfaceDesc, TSurface** lplpDDSurface, IUnknown* pUnkOuter)
	{
		if (!This || !lpDDSurfaceDesc || !lplpDDSurface)
		{
			return getOrigVtable(This).CreateSurface(This, lpDDSurfaceDesc, lplpDDSurface, pUnkOuter);
		}

		HRESULT result = DD_OK;
		if (lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)
		{
			result = DDraw::PrimarySurface::create<TDirectDraw>(*This, *lpDDSurfaceDesc, *lplpDDSurface);
		}
		else
		{
			result = DDraw::Surface::create<TDirectDraw>(
				*This, *lpDDSurfaceDesc, *lplpDDSurface, std::make_unique<DDraw::Surface>(lpDDSurfaceDesc->ddsCaps.dwCaps));
		}

		if (Compat::Log::isVerbose())
		{
			logSurfaceCreation(*lpDDSurfaceDesc, result);
		}
		return result;
	}

	template <typename TDirectDraw>
	HRESULT STDMETHODCALLTYPE SetCooperativeLevel(TDirectDraw* This, HWND hWnd, DWORD dwFlags)
	{
		HRESULT result = getOrigVtable(This).SetCooperativeLevel(This, hWnd, dwFlags);
		LOG_VERBOSE << "SetCooperativeLevel: " << ((dwFlags & DDSCL_FULLSCREEN) ? "fullscreen" : "windowed")
			<< ((dwFlags & DDSCL_EXCLUSIVE) ? ", exclusive" : "")
			<< ((dwFlags & DDSCL_MULTITHREADED) ? ", multithreaded" : "")
			<< ((dwFlags & DDSCL_FPUPRESERVE) ? ", FPU preserve" : "")
			<< ", flags " << Compat::hex(dwFlags) << ", result " << Compat::hex(result);
		return result;
	}

	template <typename TDirectDraw, typename... Params>
	HRESULT STDMETHODCALLTYPE SetDisplayMode(TDirectDraw* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP, Params... params)
	{
		HRESULT result = getOrigVtable(This).SetDisplayMode(This, dwWidth, dwHeight, dwBPP, params...);
		LOG_VERBOSE << "SetDisplayMode: " << dwWidth << 'x' << dwHeight << ", " << dwBPP << "-bit"
			<< ", result " << Compat::hex(result);
		return result;
	}

	template <typename TDirectDraw>
	HRESULT STDMETHODCALLTYPE FlipToGDISurface(TDirectDraw* /*This*/)
	{
		return DDraw::PrimarySurface::flipToGdiSurface();
	}

	template <typename TDirectDraw, typename TSurface>
	HRESULT STDMETHODCALLTYPE GetGDISurface(TDirectDraw* /*This*/, TSurface** lplpGDIDDSSurface)
	{
		if (!lplpGDIDDSSurface)
		{
			return DDERR_INVALIDPARAMS;
		}

		auto gdiSurface(DDraw::PrimarySurface::getGdiSurface());
		if (!gdiSurface)
		{
			return DDERR_NOTFOUND;
		}

		*lplpGDIDDSSurface = CompatPtr<TSurface>::from(gdiSurface.get()).detach();
		return DD_OK;
	}

	void overrideVidMem(DWORD& total, DWORD& free)
	{
		const DWORD size = Config::videoMemory * 1024 * 1024;
		total = size;
		if (free > size)
		{
			free = size;
		}
	}

	template <typename TDirectDraw, typename TDdsCaps>
	HRESULT STDMETHODCALLTYPE GetAvailableVidMem(TDirectDraw* This, TDdsCaps* lpDDSCaps, LPDWORD lpdwTotal, LPDWORD lpdwFree)
	{
		HRESULT result = getOrigVtable(This).GetAvailableVidMem(This, lpDDSCaps, lpdwTotal, lpdwFree);
		if (SUCCEEDED(result) && 0 != Config::videoMemory)
		{
			DWORD total = lpdwTotal ? *lpdwTotal : 0;
			DWORD free = lpdwFree ? *lpdwFree : 0;
			overrideVidMem(total, free);
			if (lpdwTotal)
			{
				*lpdwTotal = total;
			}
			if (lpdwFree)
			{
				*lpdwFree = free;
			}
		}
		return result;
	}

	template <typename TDirectDraw>
	HRESULT STDMETHODCALLTYPE GetCaps(TDirectDraw* This, LPDDCAPS lpDDDriverCaps, LPDDCAPS lpDDHELCaps)
	{
		HRESULT result = getOrigVtable(This).GetCaps(This, lpDDDriverCaps, lpDDHELCaps);
		if (SUCCEEDED(result) && 0 != Config::videoMemory && lpDDDriverCaps)
		{
			// dwVidMemTotal/dwVidMemFree are at the same offset in every DDCAPS version.
			overrideVidMem(lpDDDriverCaps->dwVidMemTotal, lpDDDriverCaps->dwVidMemFree);
		}
		return result;
	}

	template <typename TDirectDraw>
	HRESULT STDMETHODCALLTYPE Initialize(TDirectDraw* This, GUID* lpGUID)
	{
		logComInstantiation();
		DDraw::DirectDraw::suppressEmulatedDirectDraw(lpGUID);
		return getOrigVtable(This).Initialize(This, lpGUID);
	}

	template <typename TDirectDraw>
	HRESULT STDMETHODCALLTYPE RestoreAllSurfaces(TDirectDraw* This)
	{
		auto primary(DDraw::PrimarySurface::getPrimary());
		if (primary)
		{
			primary.get()->lpVtbl->Restore(primary);
		}
		return getOrigVtable(This).RestoreAllSurfaces(This);
	}

	template <typename TDirectDraw>
	HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(TDirectDraw* This, DWORD dwFlags, HANDLE hEvent)
	{
		if (!This || (DDWAITVB_BLOCKBEGIN != dwFlags && DDWAITVB_BLOCKEND != dwFlags))
		{
			return getOrigVtable(This).WaitForVerticalBlank(This, dwFlags, hEvent);
		}

		DWORD scanLine = 0;
		if (DDERR_VERTICALBLANKINPROGRESS != getOrigVtable(This).GetScanLine(This, &scanLine))
		{
			D3dDdi::KernelModeThunks::waitForVsync();
		}

		if (DDWAITVB_BLOCKEND == dwFlags)
		{
			while (DDERR_VERTICALBLANKINPROGRESS == getOrigVtable(This).GetScanLine(This, &scanLine));
		}

		return DD_OK;
	}

	template <typename Vtable>
	constexpr void setCompatVtable(Vtable& vtable)
	{
		vtable.CreateSurface = &CreateSurface;
		vtable.FlipToGDISurface = &FlipToGDISurface;
		vtable.GetCaps = &GetCaps;
		vtable.GetGDISurface = &GetGDISurface;
		vtable.Initialize = &Initialize;
		vtable.SetCooperativeLevel = &SetCooperativeLevel;
		vtable.SetDisplayMode = &SetDisplayMode;
		vtable.WaitForVerticalBlank = &WaitForVerticalBlank;

		if constexpr (!std::is_same_v<Vtable, IDirectDrawVtbl>)
		{
			vtable.GetAvailableVidMem = &GetAvailableVidMem;
		}

		if constexpr (std::is_same_v<Vtable, IDirectDraw4Vtbl> || std::is_same_v<Vtable, IDirectDraw7Vtbl>)
		{
			vtable.RestoreAllSurfaces = &RestoreAllSurfaces;
		}
	}
}

namespace DDraw
{
	namespace DirectDraw
	{
		DDSURFACEDESC2 getDisplayMode(CompatRef<IDirectDraw7> dd)
		{
			DDSURFACEDESC2 dm = {};
			dm.dwSize = sizeof(dm);
			dd->GetDisplayMode(&dd, &dm);
			return dm;
		}

		DDPIXELFORMAT getRgbPixelFormat(DWORD bpp)
		{
			DDPIXELFORMAT pf = {};
			pf.dwSize = sizeof(pf);
			pf.dwFlags = DDPF_RGB;
			pf.dwRGBBitCount = bpp;

			switch (bpp)
			{
			case 1:
				pf.dwFlags |= DDPF_PALETTEINDEXED1;
				break;
			case 2:
				pf.dwFlags |= DDPF_PALETTEINDEXED2;
				break;
			case 4:
				pf.dwFlags |= DDPF_PALETTEINDEXED4;
				break;
			case 8:
				pf.dwFlags |= DDPF_PALETTEINDEXED8;
				break;
			case 16:
				pf.dwRBitMask = 0xF800;
				pf.dwGBitMask = 0x07E0;
				pf.dwBBitMask = 0x001F;
				break;
			case 24:
			case 32:
				pf.dwRBitMask = 0xFF0000;
				pf.dwGBitMask = 0x00FF00;
				pf.dwBBitMask = 0x0000FF;
				break;
			}

			return pf;
		}

		void logSurfaceSummary()
		{
			for (const auto& surface : g_surfaceCounts)
			{
				Compat::Log() << "Surfaces created: " << surface.second << " x " << surface.first;
			}
		}

		void suppressEmulatedDirectDraw(GUID*& guid)
		{
			if (reinterpret_cast<GUID*>(DDCREATE_EMULATIONONLY) == guid)
			{
				LOG_ONCE("Suppressed a request to create an emulated DirectDraw object");
				guid = nullptr;
			}
		}

		template <typename Vtable>
		void hookVtable(const Vtable& vtable)
		{
			CompatVtable<Vtable>::hookVtable<ScopedThreadLock>(vtable);
		}

		template void hookVtable(const IDirectDrawVtbl&);
		template void hookVtable(const IDirectDraw2Vtbl&);
		template void hookVtable(const IDirectDraw4Vtbl&);
		template void hookVtable(const IDirectDraw7Vtbl&);
	}
}
