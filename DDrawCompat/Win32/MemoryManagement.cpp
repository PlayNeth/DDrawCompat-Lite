#include <Windows.h>

#include <Common/Hook.h>
#include <Common/Log.h>
#include <Config/Config.h>
#include <Win32/MemoryManagement.h>

namespace
{
	void limitTo2Gb(SIZE_T& mem);

	template <typename T>
	void overridePhysicalMemory(T& total, T& avail)
	{
		if (0 != Config::systemMemory)
		{
			const T size = static_cast<T>(Config::systemMemory) * 1024 * 1024;
			total = size;
			if (avail > size)
			{
				avail = size;
			}
		}
	}

	void WINAPI globalMemoryStatus(LPMEMORYSTATUS lpBuffer)
	{
		LOG_FUNC("GlobalMemoryStatus", lpBuffer);
		CALL_ORIG_FUNC(GlobalMemoryStatus)(lpBuffer);
		overridePhysicalMemory(lpBuffer->dwTotalPhys, lpBuffer->dwAvailPhys);
		if (0 != Config::systemMemory && lpBuffer->dwTotalPhys)
		{
			lpBuffer->dwMemoryLoad = static_cast<DWORD>(
				100 - static_cast<ULONGLONG>(lpBuffer->dwAvailPhys) * 100 / lpBuffer->dwTotalPhys);
		}
		limitTo2Gb(lpBuffer->dwTotalPhys);
		limitTo2Gb(lpBuffer->dwAvailPhys);
		limitTo2Gb(lpBuffer->dwTotalPageFile);
		limitTo2Gb(lpBuffer->dwAvailPageFile);
		limitTo2Gb(lpBuffer->dwTotalVirtual);
		limitTo2Gb(lpBuffer->dwAvailVirtual);
	}

	BOOL WINAPI globalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer)
	{
		LOG_FUNC("GlobalMemoryStatusEx", lpBuffer);
		BOOL result = CALL_ORIG_FUNC(GlobalMemoryStatusEx)(lpBuffer);
		if (result)
		{
			overridePhysicalMemory(lpBuffer->ullTotalPhys, lpBuffer->ullAvailPhys);
			if (0 != Config::systemMemory && lpBuffer->ullTotalPhys)
			{
				lpBuffer->dwMemoryLoad = static_cast<DWORD>(
					100 - lpBuffer->ullAvailPhys * 100 / lpBuffer->ullTotalPhys);
			}
		}
		return LOG_RESULT(result);
	}

	void limitTo2Gb(SIZE_T& mem)
	{
		if (mem > INT_MAX)
		{
			mem = INT_MAX;
		}
	}
}

namespace Win32
{
	namespace MemoryManagement
	{
		void installHooks()
		{
			HOOK_FUNCTION(kernel32, GlobalMemoryStatus, globalMemoryStatus);
			if (0 != Config::systemMemory)
			{
				HOOK_FUNCTION(kernel32, GlobalMemoryStatusEx, globalMemoryStatusEx);
			}
		}
	}
}
