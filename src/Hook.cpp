#include "Hook.h"

#include <chrono>
#include <thread>

uintptr_t GameModuleBase = (uintptr_t)GetModuleHandleA("ac4bfsp.exe");

namespace Hook
{
	// Patch a 5-byte relative JMP at pAddress, NOP-filling up to dwLen. Returns 1 on success, 0 if the
	// page could not be made writable.
	int DetourFunc(BYTE* pAddress, DWORD dwJumpTo, DWORD dwLen)
	{
		if (!pAddress || dwLen < 5) return 0;

		DWORD oldProtect = 0;
		if (!VirtualProtect(pAddress, dwLen, PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;

		pAddress[0] = 0xE9;                                   // JMP rel32
		DWORD rel = (DWORD)(dwJumpTo - (DWORD)(uintptr_t)pAddress) - 5;
		*(DWORD*)(pAddress + 1) = rel;
		for (DWORD i = 5; i < dwLen; ++i) pAddress[i] = 0x90; // NOP padding

		DWORD ignored = 0;
		VirtualProtect(pAddress, dwLen, oldProtect, &ignored);
		return 1;
	}

	void RestoreFunction(uintptr_t address, uintptr_t size, BYTE* values)
	{
		if (!address || !size || !values) return;

		BYTE* p = (BYTE*)address;
		DWORD oldProtect = 0;
		if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return;

		for (uintptr_t i = 0; i < size; ++i) p[i] = values[i];

		DWORD ignored = 0;
		VirtualProtect(p, size, oldProtect, &ignored);
	}

	void PreciseSleep(unsigned int microseconds)
	{
		// Ask for 1 ms timer resolution once; the spin below then honours sub-millisecond waits.
		static bool raised = false;
		if (!raised) {
			TIMECAPS caps = {};
			if (timeGetDevCaps(&caps, sizeof(caps)) == TIMERR_NOERROR)
				timeBeginPeriod(caps.wPeriodMin ? caps.wPeriodMin : 1);
			raised = true;
		}

		auto deadline = std::chrono::high_resolution_clock::now()
			+ std::chrono::microseconds(microseconds);
		while (std::chrono::high_resolution_clock::now() < deadline)
			std::this_thread::yield();
	}
}
