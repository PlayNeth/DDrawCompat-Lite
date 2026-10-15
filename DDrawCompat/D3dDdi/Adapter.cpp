#include <algorithm>
#include <vector>

#include <Common/CompatVtable.h>
#include <Config/Config.h>
#include <D3dDdi/Adapter.h>
#include <D3dDdi/AdapterFuncs.h>
#include <D3dDdi/Device.h>
#include <D3dDdi/DeviceCallbacks.h>
#include <D3dDdi/DeviceFuncs.h>

namespace
{
	std::string bitDepthsToString(DWORD bitDepths)
	{
		std::string result;
		if (bitDepths & DDBD_8) { result += ", 8"; }
		if (bitDepths & DDBD_16) { result += ", 16"; }
		if (bitDepths & DDBD_24) { result += ", 24"; }
		if (bitDepths & DDBD_32) { result += ", 32"; }

		if (result.empty())
		{
			return "\"\"";
		}
		return '"' + result.substr(2) + '"';
	}
}

namespace D3dDdi
{
	Adapter::Adapter(const D3DDDIARG_OPENADAPTER& data)
		: m_adapter(data.hAdapter)
		, m_origVtable(CompatVtable<D3DDDI_ADAPTERFUNCS>::s_origVtable)
		, m_runtimeVersion(data.Version)
		, m_driverVersion(data.DriverVersion)
		, m_d3dExtendedCaps{}
		, m_isP8FormatMissing(-1)
	{
		if (m_adapter)
		{
			D3DDDIARG_GETCAPS getCaps = {};
			getCaps.Type = D3DDDICAPS_GETD3D7CAPS;
			getCaps.pData = &m_d3dExtendedCaps;
			getCaps.DataSize = sizeof(m_d3dExtendedCaps);
			m_origVtable.pfnGetCaps(m_adapter, &getCaps);

			getCaps.Type = D3DDDICAPS_DDRAW;
			getCaps.pData = &m_ddrawCaps;
			getCaps.DataSize = sizeof(m_ddrawCaps);
			m_origVtable.pfnGetCaps(m_adapter, &getCaps);
		}
	}

	DWORD Adapter::getSupportedZBufferBitDepths()
	{
		const auto formatOp = getFormatOps();
		const UINT formatCount = formatOp.size();

		DWORD supportedZBufferBitDepths = 0;
		for (UINT i = 0; i < formatCount; ++i)
		{
			if (formatOp[i].Operations & (FORMATOP_ZSTENCIL | FORMATOP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH))
			{
				switch (formatOp[i].Format)
				{
				case D3DDDIFMT_D16:
					supportedZBufferBitDepths |= DDBD_16;
					break;

				case D3DDDIFMT_X8D24:
					supportedZBufferBitDepths |= DDBD_24;
					break;

				case D3DDDIFMT_D32:
					supportedZBufferBitDepths |= DDBD_32;
					break;
				}
			}
		}

		return supportedZBufferBitDepths;
	}

	HRESULT Adapter::pfnCloseAdapter()
	{
		auto adapter = m_adapter;
		auto pfnCloseAdapter = m_origVtable.pfnCloseAdapter;
		s_adapters.erase(adapter);
		return pfnCloseAdapter(adapter);
	}

	HRESULT Adapter::pfnCreateDevice(D3DDDIARG_CREATEDEVICE* pCreateData)
	{
		DeviceCallbacks::hookVtable(*pCreateData->pCallbacks, m_runtimeVersion);
		HRESULT result = m_origVtable.pfnCreateDevice(m_adapter, pCreateData);
		if (SUCCEEDED(result))
		{
			DeviceFuncs::hookVtable(*pCreateData->pDeviceFuncs, m_driverVersion);
			Device::add(*this, pCreateData->hDevice);
		}
		return result;
	}

	std::vector<FORMATOP> Adapter::getFormatOps()
	{
		UINT formatCount = 0;
		D3DDDIARG_GETCAPS caps = {};
		caps.Type = D3DDDICAPS_GETFORMATCOUNT;
		caps.pData = &formatCount;
		caps.DataSize = sizeof(formatCount);
		m_origVtable.pfnGetCaps(m_adapter, &caps);

		std::vector<FORMATOP> formatOps(formatCount);
		caps.Type = D3DDDICAPS_GETFORMATDATA;
		caps.pData = formatOps.data();
		caps.DataSize = formatCount * sizeof(FORMATOP);
		m_origVtable.pfnGetCaps(m_adapter, &caps);
		return formatOps;
	}

	bool Adapter::isP8FormatMissing()
	{
		if (m_isP8FormatMissing < 0)
		{
			const auto formatOps = getFormatOps();
			m_isP8FormatMissing = std::none_of(formatOps.begin(), formatOps.end(),
				[](const FORMATOP& op) { return D3DDDIFMT_P8 == op.Format; }) ? 1 : 0;
		}
		return 1 == m_isP8FormatMissing;
	}

	HRESULT Adapter::pfnGetCaps(const D3DDDIARG_GETCAPS* pData)
	{
		if (Config::palettizedTextures && D3DDDICAPS_GETFORMATDATA == pData->Type && isP8FormatMissing())
		{
			// The driver doesn't list 8-bit palettized surfaces at all: append an entry for emulated textures.
			const UINT count = pData->DataSize / sizeof(FORMATOP);
			if (0 == count)
			{
				return m_origVtable.pfnGetCaps(m_adapter, pData);
			}

			D3DDDIARG_GETCAPS caps = *pData;
			caps.DataSize = (count - 1) * sizeof(FORMATOP);
			HRESULT result = m_origVtable.pfnGetCaps(m_adapter, &caps);
			if (SUCCEEDED(result))
			{
				FORMATOP& op = static_cast<FORMATOP*>(pData->pData)[count - 1];
				op = {};
				op.Format = D3DDDIFMT_P8;
				op.Operations = FORMATOP_TEXTURE;
			}
			return result;
		}

		HRESULT result = m_origVtable.pfnGetCaps(m_adapter, pData);
		if (FAILED(result))
		{
			return result;
		}

		if (Config::palettizedTextures && D3DDDICAPS_GETFORMATCOUNT == pData->Type && isP8FormatMissing())
		{
			++*static_cast<UINT*>(pData->pData);
		}

		switch (pData->Type)
		{
		case D3DDDICAPS_DDRAW:
			static_cast<DDRAW_CAPS*>(pData->pData)->FxCaps =
				DDRAW_FXCAPS_BLTMIRRORLEFTRIGHT | DDRAW_FXCAPS_BLTMIRRORUPDOWN;
			break;

		case D3DDDICAPS_GETFORMATDATA:
			if (Config::palettizedTextures)
			{
				// Advertise 8-bit palettized textures; they are emulated with 32-bit ones (see Resource).
				const UINT count = pData->DataSize / sizeof(FORMATOP);
				auto formatOp = static_cast<FORMATOP*>(pData->pData);
				for (UINT i = 0; i < count; ++i)
				{
					if (D3DDDIFMT_P8 == formatOp[i].Format)
					{
						formatOp[i].Operations |= FORMATOP_TEXTURE;
						formatOp[i].Operations &= ~(FORMATOP_CUBETEXTURE | FORMATOP_VOLUMETEXTURE);
					}
				}
			}
			break;

		case D3DDDICAPS_GETD3D3CAPS:
		{
			auto& caps = static_cast<D3DNTHAL_GLOBALDRIVERDATA*>(pData->pData)->hwCaps;
			if (Config::palettizedTextures)
			{
				caps.dpcTriCaps.dwTextureCaps &= ~D3DPTEXTURECAPS_ALPHAPALETTE;
			}
			if (caps.dwFlags & D3DDD_DEVICEZBUFFERBITDEPTH)
			{
				const DWORD supportedZBufferBitDepths = getSupportedZBufferBitDepths();
				if (supportedZBufferBitDepths != caps.dwDeviceZBufferBitDepth)
				{
					LOG_ONCE("Incorrect z-buffer bit depth capabilities detected; changed from "
						<< bitDepthsToString(caps.dwDeviceZBufferBitDepth) << " to "
						<< bitDepthsToString(supportedZBufferBitDepths));
					caps.dwDeviceZBufferBitDepth = supportedZBufferBitDepths;
				}
			}
			break;
		}
		}

		return result;
	}

	std::map<HANDLE, Adapter> Adapter::s_adapters;
}
