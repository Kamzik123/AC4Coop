// Windowed mode for AC4: force the display flag off and rewrite the window style at creation.
#include "windowed.h"
#include "scimitarCommandLineOptions.h"
#include "paths.h"
#include <windows.h>
#include <cstdint>

namespace Windowed {

// scimitar::CreateWindow, module base + 0x65A0:
//     esi = (byte_2AC07D3 ? 0x7F360000 : 0)   <- preloaded from the fullscreen flag
//     add esi, 0x00CA0000                     <- 0xCA0000 windowed, 0x80000000 fullscreen popup
//     push esi -> CreateWindowExA
// We do not add to it, we replace esi outright, so the window ends up windowed whatever the flag says at
// that moment. AC4's "fullscreen" is a WS_POPUP window, not a mode switch, so the style is all that matters.
static const uintptr_t RVA_CreateWindowStyle = 0x65A0;
static const uintptr_t RVA_FullscreenFlag    = 0x26C07D3; // byte at file VA 0x2AC07D3
static const size_t    STEAL_LEN             = 6;

static uint32_t g_jmpBack    = 0;  // resume address after the stolen instruction
static uint32_t g_borderless = 0;

// The engine parses its command line after we attach and would overwrite the options struct, so a single
// write races it. Re-assert both the option and the flag for a few seconds, which spans the parse and the
// display init.
static DWORD WINAPI ForceWindowedLoop(LPVOID param)
{
    DWORD durationMs = (DWORD)(uintptr_t)param;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    scimitarCommandLineOptions* opt = scimitarCommandLineOptions::GetInstance();
    volatile uint8_t* fsFlag = reinterpret_cast<volatile uint8_t*>(base + RVA_FullscreenFlag);

    DWORD start = GetTickCount();
    do {
        opt->m_FullScreen = false;
        *fsFlag = 0;
        Sleep(15);
    } while (GetTickCount() - start < durationMs);

    OutputDebugStringA("[AC4Coop] ForceWindowedLoop finished.\n");
    return 0;
}

// Replaces esi at the CreateWindow site: bordered or borderless, then resumes the stolen code.
static void __declspec(naked) StyleDetour()
{
    __asm {
        cmp g_borderless, 0
        jne set_borderless
        mov esi, 0x00CA0000          // WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX
        jmp done
    set_borderless:
        mov esi, 0x80000000          // WS_POPUP
    done:
        jmp g_jmpBack
    }
}

static void PatchJump(uint8_t* at, void* to, size_t len)
{
    DWORD old = 0;
    VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;                                        // jmp rel32
    *reinterpret_cast<uint32_t*>(at + 1) =
        static_cast<uint32_t>(reinterpret_cast<uint8_t*>(to) - at - 5);
    for (size_t i = 5; i < len; ++i) at[i] = 0x90;      // nop pad
    VirtualProtect(at, len, old, &old);
}

void Apply(bool fullscreen, bool borderless)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    if (fullscreen) {
        scimitarCommandLineOptions::GetInstance()->m_FullScreen = true;
        return; // leave the game in its normal fullscreen mode
    }

    // 1) style detour
    g_borderless = borderless ? 1u : 0u;
    uintptr_t styleAddr = base + RVA_CreateWindowStyle;

    // The site must still hold '81 C6 00 00 CA 00' (add esi,0xCA0000). If it does not, either CreateWindow
    // already ran (we loaded too late) or the build differs - in both cases do not patch.
    const uint8_t expect[STEAL_LEN] = { 0x81, 0xC6, 0x00, 0x00, 0xCA, 0x00 };
    const uint8_t* here = reinterpret_cast<const uint8_t*>(styleAddr);
    bool match = true;
    for (size_t i = 0; i < STEAL_LEN; ++i) if (here[i] != expect[i]) { match = false; break; }
    char dbg[192];
    wsprintfA(dbg, "[AC4Coop] Windowed::Apply base=%p style=%p steal-match=%d fsflag=%d\n",
              (void*)base, (void*)styleAddr, match ? 1 : 0,
              *reinterpret_cast<uint8_t*>(base + RVA_FullscreenFlag));
    OutputDebugStringA(dbg);
    if (match) {
        g_jmpBack = static_cast<uint32_t>(styleAddr + STEAL_LEN);
        PatchJump(reinterpret_cast<uint8_t*>(styleAddr), reinterpret_cast<void*>(&StyleDetour), STEAL_LEN);
    }

    // 2) keep re-asserting windowed while the engine parses its command line.
    //    Duration: [Display] ForceWindowedMs (default 5000).
    DWORD durationMs = GetPrivateProfileIntA("Display", "ForceWindowedMs", 5000, Paths::Ini());
    CreateThread(nullptr, 0, ForceWindowedLoop, (LPVOID)(uintptr_t)durationMs, 0, nullptr);
}

} // namespace Windowed
