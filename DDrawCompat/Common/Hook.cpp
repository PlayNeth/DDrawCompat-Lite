#undef CINTERFACE

#include <list>
#include <sstream>
#include <string>

#include <Windows.h>

#include <Common/Disasm.h>
#include <Common/Hook.h>
#include <Common/Log.h>
#include <Common/Path.h>
#include <Dll/Dll.h>

namespace
{
	PIMAGE_NT_HEADERS getImageNtHeaders(HMODULE module);

	FARPROC* findProcAddressInIat(HMODULE module, const char* procName)
	{
		if (!module || !procName)
		{
			return nullptr;
		}

		PIMAGE_NT_HEADERS ntHeaders = getImageNtHeaders(module);
		if (!ntHeaders)
		{
			return nullptr;
		}

		char* moduleBase = reinterpret_cast<char*>(module);
		PIMAGE_IMPORT_DESCRIPTOR importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(moduleBase +
			ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

		for (PIMAGE_IMPORT_DESCRIPTOR desc = importDesc;
			0 != desc->Characteristics && 0xFFFF != desc->Name;
			++desc)
		{
			auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(moduleBase + desc->FirstThunk);
			auto origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(moduleBase + desc->OriginalFirstThunk);
			while (0 != thunk->u1.AddressOfData && 0 != origThunk->u1.AddressOfData)
			{
				auto origImport = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
					moduleBase + origThunk->u1.AddressOfData);

				if (0 == strcmp(origImport->Name, procName))
				{
					return reinterpret_cast<FARPROC*>(&thunk->u1.Function);
				}

				++thunk;
				++origThunk;
			}
		}

		return nullptr;
	}

	PIMAGE_NT_HEADERS getImageNtHeaders(HMODULE module)
	{
		PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
		if (IMAGE_DOS_SIGNATURE != dosHeader->e_magic)
		{
			return nullptr;
		}

		PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
			reinterpret_cast<char*>(dosHeader) + dosHeader->e_lfanew);
		if (IMAGE_NT_SIGNATURE != ntHeaders->Signature)
		{
			return nullptr;
		}

		return ntHeaders;
	}

	// All trampolines share one executable block instead of a 64 KB address space reservation each.
	BYTE* allocTrampoline(unsigned size)
	{
		const unsigned BLOCK_SIZE = 64 * 1024;
		static BYTE* block = nullptr;
		static unsigned used = BLOCK_SIZE;
		static SRWLOCK lock = SRWLOCK_INIT;

		size = (size + 15) & ~15;
		AcquireSRWLockExclusive(&lock);
		if (used + size > BLOCK_SIZE)
		{
			block = static_cast<BYTE*>(VirtualAlloc(nullptr, BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			used = 0;
		}
		BYTE* trampoline = block ? block + used : nullptr;
		used += size;
		ReleaseSRWLockExclusive(&lock);
		return trampoline;
	}

	// Fixes up relative branch targets in instructions copied from src to dst (the trampoline).
	// rel32 call/jmp/jcc are adjusted; short (rel8) branches cannot reach back and are rejected.
	bool relocateInstructions(BYTE* dst, const BYTE* src, unsigned size)
	{
		const int delta = static_cast<int>(src - dst);
		unsigned offset = 0;
		while (offset < size)
		{
			const unsigned length = Compat::getInstructionLength(src + offset);
			if (0 == length)
			{
				return false;
			}

			const BYTE op = src[offset];
			if ((0xE8 == op || 0xE9 == op) && 5 == length)
			{
				reinterpret_cast<int&>(dst[offset + 1]) += delta;
			}
			else if (0x0F == op && src[offset + 1] >= 0x80 && src[offset + 1] <= 0x8F && 6 == length)
			{
				reinterpret_cast<int&>(dst[offset + 2]) += delta;
			}
			else if (0xEB == op || (op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3))
			{
				return false;
			}
			offset += length;
		}
		return true;
	}

	void hookFunction(void*& origFuncPtr, void* newFuncPtr, const char* funcName)
	{
		BYTE* targetFunc = reinterpret_cast<BYTE*>(origFuncPtr);

		std::ostringstream oss;
#ifdef DEBUGLOGS
		oss << Compat::funcPtrToStr(targetFunc) << ' ';

		char origFuncPtrStr[20] = {};
		if (!funcName)
		{
			sprintf_s(origFuncPtrStr, "%p", origFuncPtr);
			funcName = origFuncPtrStr;
		}

		auto prevTargetFunc = targetFunc;
#endif

		while (true)
		{
			unsigned instructionSize = 0;
			if (0xE9 == targetFunc[0])
			{
				instructionSize = 5;
				targetFunc += instructionSize + *reinterpret_cast<int*>(targetFunc + 1);
			}
			else if (0xEB == targetFunc[0])
			{
				instructionSize = 2;
				targetFunc += instructionSize + *reinterpret_cast<signed char*>(targetFunc + 1);
			}
			else if (0xFF == targetFunc[0] && 0x25 == targetFunc[1])
			{
				instructionSize = 6;
				targetFunc = **reinterpret_cast<BYTE***>(targetFunc + 2);
			}
			else
			{
				break;
			}
#ifdef DEBUGLOGS
			oss << Compat::hexDump(prevTargetFunc, instructionSize) << " -> " << Compat::funcPtrToStr(targetFunc) << ' ';
			prevTargetFunc = targetFunc;
#endif
		}

		if (Compat::getModuleHandleFromAddress(targetFunc) == Dll::g_currentModule)
		{
			Compat::Log() << "ERROR: Target function is already hooked: " << funcName;
			return;
		}

		unsigned totalInstructionSize = 0;
		while (totalInstructionSize < 5)
		{
			unsigned instructionSize = Compat::getInstructionLength(targetFunc + totalInstructionSize);
			if (0 == instructionSize)
			{
				return;
			}
			totalInstructionSize += instructionSize;
		}

		LOG_DEBUG << "Hooking function: " << funcName
			<< " (" << oss.str() << Compat::hexDump(targetFunc, totalInstructionSize) << ')';

		BYTE* trampoline = allocTrampoline(totalInstructionSize + 5);
		if (!trampoline)
		{
			Compat::Log() << "ERROR: Failed to allocate a trampoline for function: " << funcName;
			return;
		}

		memcpy(trampoline, targetFunc, totalInstructionSize);
		if (!relocateInstructions(trampoline, targetFunc, totalInstructionSize))
		{
			Compat::Log() << "ERROR: Cannot relocate the start of function: " << funcName;
			return;
		}
		trampoline[totalInstructionSize] = 0xE9;
		reinterpret_cast<int&>(trampoline[totalInstructionSize + 1]) = targetFunc - (trampoline + 5);
		DWORD oldProtect = 0;

		VirtualProtect(targetFunc, totalInstructionSize, PAGE_EXECUTE_READWRITE, &oldProtect);
		targetFunc[0] = 0xE9;
		reinterpret_cast<int&>(targetFunc[1]) = static_cast<BYTE*>(newFuncPtr) - (targetFunc + 5);
		memset(targetFunc + 5, 0xCC, totalInstructionSize - 5);
		VirtualProtect(targetFunc, totalInstructionSize, PAGE_EXECUTE_READ, &oldProtect);

		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

		HMODULE module = nullptr;
		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<char*>(targetFunc), &module);

		origFuncPtr = trampoline;
	}
}

namespace Compat
{
	std::string funcPtrToStr(void* funcPtr)
	{
		std::ostringstream oss;
		HMODULE module = Compat::getModuleHandleFromAddress(funcPtr);
		if (module)
		{
			oss << Compat::getModulePath(module).u8string() << "+0x" << std::hex <<
				reinterpret_cast<DWORD>(funcPtr) - reinterpret_cast<DWORD>(module);
		}
		else
		{
			oss << funcPtr;
		}
		return oss.str();
	}

	HMODULE getModuleHandleFromAddress(void* address)
	{
		HMODULE module = nullptr;
		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<char*>(address), &module);
		return module;
	}

	FARPROC getProcAddress(HMODULE module, const char* procName)
	{
		if (!module || !procName)
		{
			return nullptr;
		}

		PIMAGE_NT_HEADERS ntHeaders = getImageNtHeaders(module);
		if (!ntHeaders)
		{
			return nullptr;
		}

		char* moduleBase = reinterpret_cast<char*>(module);
		PIMAGE_EXPORT_DIRECTORY exportDir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(
			moduleBase + ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
		auto exportDirSize = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;

		DWORD* rvaOfNames = reinterpret_cast<DWORD*>(moduleBase + exportDir->AddressOfNames);
		WORD* nameOrds = reinterpret_cast<WORD*>(moduleBase + exportDir->AddressOfNameOrdinals);
		DWORD* rvaOfFunctions = reinterpret_cast<DWORD*>(moduleBase + exportDir->AddressOfFunctions);

		char* func = nullptr;
		if (0 == HIWORD(procName))
		{
			WORD ord = LOWORD(procName);
			if (ord < exportDir->Base || ord >= exportDir->Base + exportDir->NumberOfFunctions)
			{
				return nullptr;
			}
			func = moduleBase + rvaOfFunctions[ord - exportDir->Base];
		}
		else
		{
			for (DWORD i = 0; i < exportDir->NumberOfNames; ++i)
			{
				if (0 == strcmp(procName, moduleBase + rvaOfNames[i]))
				{
					func = moduleBase + rvaOfFunctions[nameOrds[i]];
				}
			}
		}

		if (func &&
			func >= reinterpret_cast<char*>(exportDir) &&
			func < reinterpret_cast<char*>(exportDir) + exportDirSize)
		{
			std::string forw(func);
			auto separatorPos = forw.find_first_of('.');
			if (std::string::npos == separatorPos)
			{
				return nullptr;
			}
			HMODULE forwModule = GetModuleHandle(forw.substr(0, separatorPos).c_str());
			std::string forwFuncName = forw.substr(separatorPos + 1);
			if ('#' == forwFuncName[0])
			{
				int32_t ord = std::atoi(forwFuncName.substr(1).c_str());
				if (ord < 0 || ord > 0xFFFF)
				{
					return nullptr;
				}
				return getProcAddress(forwModule, reinterpret_cast<const char*>(ord));
			}
			else
			{
				return getProcAddress(forwModule, forwFuncName.c_str());
			}
		}

		return reinterpret_cast<FARPROC>(func);
	}

	void hookFunction(void*& origFuncPtr, void* newFuncPtr, const char* funcName)
	{
		::hookFunction(origFuncPtr, newFuncPtr, funcName);
	}

	void hookFunction(HMODULE module, const char* funcName, void*& origFuncPtr, void* newFuncPtr)
	{
		FARPROC procAddr = getProcAddress(module, funcName);
		if (!procAddr)
		{
			LOG_DEBUG << "ERROR: Failed to load the address of a function: " << funcName;
			return;
		}

		origFuncPtr = procAddr;
		::hookFunction(origFuncPtr, newFuncPtr, funcName);
	}

	void hookFunction(const char* moduleName, const char* funcName, void*& origFuncPtr, void* newFuncPtr)
	{
		HMODULE module = LoadLibrary(moduleName);
		if (!module)
		{
			return;
		}
		hookFunction(module, funcName, origFuncPtr, newFuncPtr);
		FreeLibrary(module);
	}

	void hookIatFunction(HMODULE module, const char* funcName, void* newFuncPtr)
	{
		FARPROC* func = findProcAddressInIat(module, funcName);
		if (func)
		{
			LOG_DEBUG << "Hooking function via IAT: " << funcName << " (" << funcPtrToStr(*func) << ')';
			DWORD oldProtect = 0;
			VirtualProtect(func, sizeof(func), PAGE_READWRITE, &oldProtect);
			*func = static_cast<FARPROC>(newFuncPtr);
			DWORD dummy = 0;
			VirtualProtect(func, sizeof(func), oldProtect, &dummy);
		}
	}

	void removeShim(HMODULE module, const char* funcName)
	{
		void* shimFunc = GetProcAddress(module, funcName);
		if (shimFunc)
		{
			void* realFunc = getProcAddress(module, funcName);
			if (realFunc && shimFunc != realFunc)
			{
				static std::list<void*> shimFuncs;
				shimFuncs.push_back(shimFunc);
				std::string shimFuncName("[shim]");
				shimFuncName += funcName;
				hookFunction(shimFuncs.back(), realFunc, shimFuncName.c_str());
			}
		}
	}
}
