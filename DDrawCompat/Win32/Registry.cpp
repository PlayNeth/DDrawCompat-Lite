#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <Common/Hook.h>
#include <Common/Log.h>
#include <Config/Config.h>
#include <Win32/Registry.h>

typedef long NTSTATUS;

namespace
{
	struct RegistryKey
	{
		HKEY key;
		std::wstring subKey;
		std::wstring value;

		RegistryKey(HKEY key, const std::wstring& subKey, const std::wstring& value)
			: key(key), subKey(subKey), value(value)
		{
		}

		bool operator<(const RegistryKey& rhs) const
		{
			if (key < rhs.key) { return true; }
			if (key > rhs.key) { return false; }
			const int subKeyComp = lstrcmpiW(subKey.c_str(), rhs.subKey.c_str());
			if (subKeyComp < 0) { return true; }
			if (subKeyComp > 0) { return false; }
			return lstrcmpiW(value.c_str(), rhs.value.c_str()) < 0;
		}

		bool operator==(const RegistryKey& rhs) const
		{
			return key == rhs.key &&
				0 == lstrcmpiW(subKey.c_str(), rhs.subKey.c_str()) &&
				0 == lstrcmpiW(value.c_str(), rhs.value.c_str());
		}
	};

	std::map<RegistryKey, DWORD> g_dwordValues;
	std::set<RegistryKey> g_unsetValues;
	std::vector<std::string> g_unsetValueNames;

	std::wstring getKeyName(HKEY key)
	{
		enum KEY_INFORMATION_CLASS
		{
			KeyBasicInformation = 0,
			KeyNodeInformation = 1,
			KeyFullInformation = 2,
			KeyNameInformation = 3,
			KeyCachedInformation = 4,
			KeyFlagsInformation = 5,
			KeyVirtualizationInformation = 6,
			KeyHandleTagsInformation = 7,
			MaxKeyInfoClass = 8
		};

		typedef NTSTATUS(WINAPI *NtQueryKeyFuncPtr)(
			HANDLE KeyHandle,
			KEY_INFORMATION_CLASS KeyInformationClass,
			PVOID KeyInformation,
			ULONG Length,
			PULONG ResultLength);

		static NtQueryKeyFuncPtr ntQueryKey = reinterpret_cast<NtQueryKeyFuncPtr>(
			GetProcAddress(GetModuleHandle("ntdll"), "NtQueryKey"));
		
		if (ntQueryKey)
		{
			struct KEY_NAME_INFORMATION
			{
				ULONG NameLength;
				WCHAR Name[256];
			};

			KEY_NAME_INFORMATION keyName = {};
			ULONG resultSize = 0;
			if (SUCCEEDED(ntQueryKey(key, KeyNameInformation, &keyName, sizeof(keyName), &resultSize)))
			{
				return std::wstring(keyName.Name, keyName.NameLength / 2);
			}
		}

		return std::wstring();
	}

	LONG WINAPI regGetValueW(HKEY hkey, LPCWSTR lpSubKey, LPCWSTR lpValue, 
		DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
	{
		LOG_FUNC("regGetValueW", hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
		LONG result = ERROR_SUCCESS;

		const auto it = hkey && lpSubKey && lpValue && (dwFlags & RRF_RT_REG_DWORD)
			? g_dwordValues.find(RegistryKey(hkey, lpSubKey, lpValue))
			: g_dwordValues.end();

		if (it != g_dwordValues.end())
		{
			if (pdwType)
			{
				*pdwType = REG_DWORD;
			}

			if (pvData)
			{
				if (!pcbData)
				{
					result = ERROR_INVALID_PARAMETER;
				}
				else if (*pcbData >= sizeof(DWORD))
				{
					std::memcpy(pvData, &it->second, sizeof(DWORD));
				}
				else
				{
					result = ERROR_MORE_DATA;
				}
			}

			if (pcbData)
			{
				*pcbData = sizeof(DWORD);
			}
		}
		else
		{
			result = CALL_ORIG_FUNC(RegGetValueW)(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
		}

		return LOG_RESULT(result);
	}

	bool isCpuSpeedValue(HKEY hKey, const std::wstring& valueName)
	{
		if (0 == Config::cpuSpeed || !hKey || 0 != lstrcmpiW(valueName.c_str(), L"~MHz"))
		{
			return false;
		}

		std::wstring keyName = getKeyName(hKey);
		CharUpperBuffW(keyName.data(), static_cast<DWORD>(keyName.size()));
		return std::wstring::npos != keyName.find(L"\\HARDWARE\\DESCRIPTION\\SYSTEM\\CENTRALPROCESSOR\\");
	}

	LONG returnDword(DWORD value, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
	{
		if (lpData && !lpcbData)
		{
			return ERROR_INVALID_PARAMETER;
		}

		if (lpType)
		{
			*lpType = REG_DWORD;
		}

		LONG result = ERROR_SUCCESS;
		if (lpData)
		{
			if (*lpcbData >= sizeof(DWORD))
			{
				std::memcpy(lpData, &value, sizeof(DWORD));
			}
			else
			{
				result = ERROR_MORE_DATA;
			}
		}

		if (lpcbData)
		{
			*lpcbData = sizeof(DWORD);
		}
		return result;
	}

	LONG WINAPI regQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType,
		LPBYTE lpData, LPDWORD lpcbData)
	{
		LOG_FUNC("regQueryValueExA", hKey, lpValueName, lpReserved, lpType, static_cast<void*>(lpData), lpcbData);

		if (hKey && lpValueName)
		{
			// Cheap value name checks first: looking up the key name is a kernel call plus string work,
			// and this hook sees every registry read in the process.
			if (0 != Config::cpuSpeed && 0 == lstrcmpiA(lpValueName, "~MHz") && isCpuSpeedValue(hKey, L"~MHz"))
			{
				return LOG_RESULT(returnDword(Config::cpuSpeed, lpType, lpData, lpcbData));
			}

			const bool isNameMatching = std::any_of(g_unsetValueNames.begin(), g_unsetValueNames.end(),
				[&](const std::string& name) { return 0 == lstrcmpiA(name.c_str(), lpValueName); });
			if (isNameMatching)
			{
				std::wostringstream oss;
				oss << lpValueName;
				const std::wstring valueName = oss.str();
				{
					const std::wstring keyName = getKeyName(hKey);
					const std::wstring localMachinePrefix = L"\\REGISTRY\\MACHINE\\";
					if (localMachinePrefix == keyName.substr(0, localMachinePrefix.size()) &&
						g_unsetValues.end() != g_unsetValues.find(RegistryKey(HKEY_LOCAL_MACHINE,
							keyName.substr(localMachinePrefix.size()), valueName)))
					{
						return LOG_RESULT(ERROR_FILE_NOT_FOUND);
					}
				}
			}
		}

		return LOG_RESULT(CALL_ORIG_FUNC(RegQueryValueExA)(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData));
	}

	LONG WINAPI regQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType,
		LPBYTE lpData, LPDWORD lpcbData)
	{
		LOG_FUNC("regQueryValueExW", hKey, lpValueName, lpReserved, lpType, static_cast<void*>(lpData), lpcbData);

		if (hKey && lpValueName && 0 == lstrcmpiW(lpValueName, L"~MHz") && isCpuSpeedValue(hKey, lpValueName))
		{
			return LOG_RESULT(returnDword(Config::cpuSpeed, lpType, lpData, lpcbData));
		}

		return LOG_RESULT(CALL_ORIG_FUNC(RegQueryValueExW)(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData));
	}
}

namespace Win32
{
	namespace Registry
	{
		void installRegQueryValueExHooks()
		{
			static bool isAlreadyInstalled = false;
			if (isAlreadyInstalled)
			{
				return;
			}
			isAlreadyInstalled = true;

			HOOK_SHIM_FUNCTION(RegQueryValueExA, regQueryValueExA);
			if (0 != Config::cpuSpeed)
			{
				HOOK_SHIM_FUNCTION(RegQueryValueExW, regQueryValueExW);
			}
		}

		void installEarlyHooks()
		{
			// Games usually read the CPU speed at startup, before creating any DirectDraw object.
			if (0 != Config::cpuSpeed)
			{
				installRegQueryValueExHooks();
			}
		}

		void installHooks()
		{
			HOOK_SHIM_FUNCTION(RegGetValueW, regGetValueW);
			installRegQueryValueExHooks();
		}

		void setValue(HKEY key, const char* subKey, const char* valueName, DWORD value)
		{
			assert(key && subKey && valueName);
			std::wostringstream subKeyW;
			subKeyW << subKey;
			std::wostringstream valueNameW;
			valueNameW << valueName;
			g_dwordValues[RegistryKey(key, subKeyW.str(), valueNameW.str())] = value;
		}

		void unsetValue(HKEY key, const char* subKey, const char* valueName)
		{
			assert(key && subKey && valueName);
			std::wostringstream subKeyW;
			subKeyW << subKey;
			std::wostringstream valueNameW;
			valueNameW << valueName;
			g_unsetValues.insert(RegistryKey(key, subKeyW.str(), valueNameW.str()));
			g_unsetValueNames.push_back(valueName);
		}
	}
}
