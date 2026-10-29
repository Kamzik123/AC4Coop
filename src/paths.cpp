#include "paths.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

// One-time resolution. GetModuleHandleExA with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS on an address inside
// this unit returns the ASI itself -- GetModuleHandle(NULL) would return the game exe, whose directory is
// usually right but is not the module that owns the ini.
namespace {

char s_dir[MAX_PATH] = { 0 };      // absolute module directory, no trailing backslash
char s_ini[MAX_PATH] = { 0 };      // absolute ini path, or the legacy relative one
int  s_iniAtModule = 0;            // 1 = resolved next to the ASI, 0 = CWD fallback

// Per-leaf log path cache. Entries are append-only and never moved, so a returned pointer stays valid.
struct LeafEntry {
    char leaf[96];
    char path[MAX_PATH];
};
const int  LEAF_MAX = 32;
LeafEntry  s_leaves[LEAF_MAX];
int        s_leafN = 0;

CRITICAL_SECTION s_leafLock;
bool             s_locksReady = false;

INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitPaths(PINIT_ONCE, PVOID, PVOID*)
{
    if (!s_locksReady) {
        InitializeCriticalSection(&s_leafLock);
        s_locksReady = true;
    }

    char full[MAX_PATH] = { 0 };
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&Paths::ModuleDir, &self) && self) {
        const DWORD n = GetModuleFileNameA(self, full, (DWORD)MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            full[0] = 0;
        } else {
            char* slash = strrchr(full, '\\');
            if (slash) *slash = 0;       // strip the file name
            else       full[0] = 0;
        }
    }
    if (!full[0]) GetCurrentDirectoryA((DWORD)MAX_PATH, full);   // last resort, never leave the dir empty
    strncpy_s(s_dir, full, _TRUNCATE);

    char moduleIni[MAX_PATH] = { 0 };
    _snprintf_s(moduleIni, sizeof(moduleIni), _TRUNCATE, "%s\\AC4Coop.ini", s_dir);
    const DWORD attr = GetFileAttributesA(moduleIni);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        s_iniAtModule = 1;
        strncpy_s(s_ini, moduleIni, _TRUNCATE);
    } else {
        // Keep existing installs working: the old relative path produced the defaults when the CWD was
        // the game folder, and still does when the ini is absent everywhere.
        s_iniAtModule = 0;
        strncpy_s(s_ini, ".\\AC4Coop.ini", _TRUNCATE);
    }
    return TRUE;
}

void EnsureInit()
{
    InitOnceExecuteOnce(&s_once, InitPaths, nullptr, nullptr);
}

} // namespace

const char* Paths::ModuleDir()
{
    EnsureInit();
    return s_dir;
}

const char* Paths::Ini()
{
    EnsureInit();
    return s_ini;
}

int Paths::IniAtModule()
{
    EnsureInit();
    return s_iniAtModule;
}

const char* Paths::LogPath(const char* leaf)
{
    EnsureInit();
    if (!leaf) leaf = "";

    EnterCriticalSection(&s_leafLock);
    const char* out = nullptr;
    for (int i = 0; i < s_leafN; ++i) {
        if (strcmp(s_leaves[i].leaf, leaf) == 0) { out = s_leaves[i].path; break; }
    }
    if (!out) {
        if (s_leafN < LEAF_MAX) {
            LeafEntry& e = s_leaves[s_leafN++];
            strncpy_s(e.leaf, leaf, _TRUNCATE);
            _snprintf_s(e.path, sizeof(e.path), _TRUNCATE, "%s\\%s", s_dir, leaf);
            out = e.path;
        } else {
            // More distinct leaves than slots: do not alias another entry (that would silently write one
            // log over another). Logs are opened once per process, so this is unreachable in practice.
            static char overflow[MAX_PATH] = { 0 };
            _snprintf_s(overflow, sizeof(overflow), _TRUNCATE, "%s\\%s", s_dir, leaf);
            out = overflow;
        }
    }
    LeaveCriticalSection(&s_leafLock);
    return out;
}

void Paths::LogPath(char* dst, size_t cap, const char* leaf)
{
    if (!dst || cap == 0) return;
    _snprintf_s(dst, cap, _TRUNCATE, "%s", LogPath(leaf));
}

int Paths::FormatStartupLine(char* dst, size_t cap)
{
    if (!dst || cap == 0) return 0;
    EnsureInit();
    // exists=0 means the ini was not found next to the ASI and the CWD fallback is in use, i.e. any
    // setting a tester put in the ASI folder was not read.
    const int n = _snprintf_s(dst, cap, _TRUNCATE,
                              "[Paths] module=%s ini=%s exists=%d logdir=%s\r\n",
                              s_dir, s_ini, s_iniAtModule, s_dir);
    return n > 0 ? n : 0;
}
