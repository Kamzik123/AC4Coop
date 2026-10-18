#pragma once
#pragma comment(lib, "winmm.lib")

#include <Windows.h>
#include <cstdint>

// Absolute image base of AC4BFSP.exe. The engine is not ASLR-relocated here, so engine code/data
// addresses are used as absolute VAs elsewhere in the project.
extern uintptr_t GameModuleBase;

namespace Hook
{
	// Overwrite the first dwLen bytes at pAddress with a relative JMP to dwJumpTo (padded with NOPs).
	// The caller owns restoring the original bytes.
	int  DetourFunc(BYTE* pAddress, DWORD dwJumpTo, DWORD dwLen);

	// Write `size` bytes at `address` from `values` (used to undo a detour).
	void RestoreFunction(uintptr_t address, uintptr_t size, BYTE* values);

	// ---- calling engine functions by absolute address -------------------------------------------
	// These are plain function-pointer casts; no engine state is touched.
	template <typename Ret, typename C, typename... Args>
	Ret CallClassMethod(unsigned long address, C self, Args... args)
	{
		return (reinterpret_cast<Ret(__thiscall*)(C, Args...)>(address))(self, args...);
	}

	template <typename Ret, typename... Args>
	Ret CallCdeclMethod(unsigned long address, Args... args)
	{
		return (reinterpret_cast<Ret(__cdecl*)(Args...)>(address))(args...);
	}

	template <typename Ret, typename... Args>
	Ret CallStdMethod(unsigned long address, Args... args)
	{
		return (reinterpret_cast<Ret(__stdcall*)(Args...)>(address))(args...);
	}

	template <typename Ret, typename... Args>
	Ret CallFastCallMethod(unsigned long address, Args... args)
	{
		return (reinterpret_cast<Ret(__fastcall*)(Args...)>(address))(args...);
	}

	// Spin-wait on a high-resolution clock; raises the system timer resolution once.
	void PreciseSleep(unsigned int microseconds);
}
