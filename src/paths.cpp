#include "paths.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <share.h>

// One-time resolution. GetModuleHandleExA with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS on an address inside
// this unit returns the ASI itself -- GetModuleHandle(NULL) would return the game exe, whose directory is
// usually right but is not the module that owns the ini.
namespace {

char s_dir[MAX_PATH] = { 0 };      // absolute module directory, no trailing backslash
char s_ini[MAX_PATH] = { 0 };      // absolute ini path, or the legacy relative one
int  s_iniAtModule = 0;            // 1 = resolved next to the ASI, 0 = localappdata/CWD fallback
const char* s_iniFrom = "none";    // "module" | "localappdata" | "cwd" | "none"

char s_localAppData[MAX_PATH] = { 0 };   // %LOCALAPPDATA%, empty when the variable is unset
char s_logDir[MAX_PATH] = { 0 };         // directory every log file is written to
const char* s_logFrom = "none";          // "module" | "localappdata" | "temp" | "none"
int  s_moduleWritable = 0;               // module-dir write probe result
DWORD s_moduleWriteErr = 0;              // Win32 error of the failed probe (0 when it succeeded)

// Per-leaf log path cache. Entries are append-only and never moved, so a returned pointer stays valid.
struct LeafEntry {
    char leaf[96];
    char path[MAX_PATH];
};
const int  LEAF_MAX = 32;
LeafEntry  s_leaves[LEAF_MAX];
int        s_leafN = 0;

// Paths whose log open already failed this process: NoteOpenFailure reports each one only once.
const int  FAILED_MAX = 8;
char       s_failed[FAILED_MAX][MAX_PATH];
int        s_failedN = 0;

CRITICAL_SECTION s_leafLock;
bool             s_locksReady = false;

INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

// "<a>\<b>" into out. Skips the separator when `a` already ends with one (GetTempPath does).
void JoinPath(char* out, size_t cap, const char* a, const char* b)
{
    const size_t n = strlen(a);
    if (n && a[n - 1] == '\\')
        _snprintf_s(out, cap, _TRUNCATE, "%s%s", a, b);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", a, b);
}

// CreateDirectoryA for every component of an absolute path. Already-existing components are fine, which
// is the normal case for everything above the AC4Coop folder.
bool CreateDirTree(const char* path)
{
    if (!path || !path[0]) return false;
    char work[MAX_PATH] = { 0 };
    strncpy_s(work, path, _TRUNCATE);
    const size_t n = strlen(work);
    // Start past "C:" -- CreateDirectoryA("C:") means "the CWD on drive C", not the drive root.
    for (size_t i = (n >= 3 && work[1] == ':') ? 3 : 1; i <= n; ++i) {
        if (i != n && work[i] != '\\') continue;
        const char saved = work[i];
        work[i] = 0;
        if (work[0] && !CreateDirectoryA(work, nullptr)) {
            const DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) {
                // ERROR_ALREADY_EXISTS-only is fine; anything else must already be a directory.
                const DWORD at = GetFileAttributesA(work);
                if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    work[i] = saved;
                    return false;
                }
            }
        }
        work[i] = saved;
    }
    return true;
}

// The only honest writability test: create, write 4 bytes, flush, close, delete. Checks the return value
// of every step; fopen/fwrite can fail where GetFileAttributes/DirectoryExists would have said "fine"
// (which is exactly the 2026-10-29 failure: the files WERE created, nothing could be written).
bool ProbeWritable(const char* dir, DWORD* errOut)
{
    if (errOut) *errOut = 0;
    if (!dir || !dir[0]) {
        if (errOut) *errOut = ERROR_PATH_NOT_FOUND;
        return false;
    }

    char test[MAX_PATH] = { 0 };
    JoinPath(test, sizeof(test), dir, ".ac4coop_write_test");

    FILE* f = _fsopen(test, "wb", _SH_DENYNO);
    if (!f) {
        DWORD e = GetLastError();
        if (!e) e = ERROR_OPEN_FAILED;   // keep "failed, reason unstated" distinct from "succeeded"
        if (errOut) *errOut = e;
        return false;
    }

    const char probe[4] = { 'A', 'C', '4', '!' };
    DWORD e = 0;
    bool ok = fwrite(probe, 1, sizeof(probe), f) == sizeof(probe);
    if (!ok) {
        e = GetLastError();
        if (!e) e = ERROR_WRITE_FAULT;
    } else if (fflush(f) != 0) {
        ok = false;
        e = GetLastError();
        if (!e) e = ERROR_WRITE_FAULT;
    }
    fclose(f);
    DeleteFileA(test);

    if (!ok && errOut) *errOut = e;
    return ok;
}

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

    // %LOCALAPPDATA% is the per-user folder that needs no elevation -- "C:\Program Files (x86)" is not
    // writable for a normal user, which is what made every log 0 bytes. Empty means "skip this step".
    ZeroMemory(s_localAppData, sizeof(s_localAppData));
    {
        const DWORD ln = GetEnvironmentVariableA("LOCALAPPDATA", s_localAppData, (DWORD)MAX_PATH);
        if (ln == 0 || ln >= MAX_PATH) s_localAppData[0] = 0;   // unset, or too long to be used safely
    }

    // ---- ini: module -> %LOCALAPPDATA%\AC4Coop -> CWD ------------------------------------------------
    char moduleIni[MAX_PATH] = { 0 };
    JoinPath(moduleIni, sizeof(moduleIni), s_dir, "AC4Coop.ini");
    const DWORD attr = GetFileAttributesA(moduleIni);

    char appDataIni[MAX_PATH] = { 0 };
    if (s_localAppData[0]) {
        char appDataDir[MAX_PATH] = { 0 };
        JoinPath(appDataDir, sizeof(appDataDir), s_localAppData, "AC4Coop");
        JoinPath(appDataIni, sizeof(appDataIni), appDataDir, "AC4Coop.ini");
    }

    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        s_iniAtModule = 1;
        s_iniFrom = "module";
        strncpy_s(s_ini, moduleIni, _TRUNCATE);
    } else if (appDataIni[0]) {
        const DWORD a2 = GetFileAttributesA(appDataIni);
        if (a2 != INVALID_FILE_ATTRIBUTES && (a2 & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            s_iniAtModule = 0;
            s_iniFrom = "localappdata";
            strncpy_s(s_ini, appDataIni, _TRUNCATE);
        } else {
            s_iniAtModule = 0;
            s_iniFrom = "cwd";
            strncpy_s(s_ini, ".\\AC4Coop.ini", _TRUNCATE);
        }
    } else {
        // Keep existing installs working: the old relative path produced the defaults when the CWD was
        // the game folder, and still does when the ini is absent everywhere.
        char cwd[MAX_PATH] = { 0 };
        if (GetCurrentDirectoryA((DWORD)MAX_PATH, cwd) && cwd[0]) {
            s_iniAtModule = 0;
            s_iniFrom = "cwd";
            strncpy_s(s_ini, ".\\AC4Coop.ini", _TRUNCATE);
        } else {
            s_iniAtModule = 0;
            s_iniFrom = "none";
            s_ini[0] = 0;
        }
    }

    // ---- log directory: module (if provably writable) -> localappdata -> temp ------------------------
    DWORD probeErr = 0;
    s_moduleWritable = ProbeWritable(s_dir, &probeErr) ? 1 : 0;
    s_moduleWriteErr = probeErr;

    if (s_moduleWritable) {
        strncpy_s(s_logDir, s_dir, _TRUNCATE);
        s_logFrom = "module";
    }

    if (!s_logDir[0] && s_localAppData[0]) {
        char base[MAX_PATH] = { 0 };
        char dir[MAX_PATH] = { 0 };
        JoinPath(base, sizeof(base), s_localAppData, "AC4Coop");
        JoinPath(dir, sizeof(dir), base, "logs");
        DWORD e2 = 0;
        if (CreateDirTree(dir) && ProbeWritable(dir, &e2)) {
            strncpy_s(s_logDir, dir, _TRUNCATE);
            s_logFrom = "localappdata";
        }
    }

    if (!s_logDir[0]) {
        char tmp[MAX_PATH] = { 0 };
        const DWORD tn = GetTempPathA((DWORD)MAX_PATH, tmp);
        if (tn > 0 && tn < MAX_PATH && tmp[0]) {
            char base[MAX_PATH] = { 0 };
            char dir[MAX_PATH] = { 0 };
            JoinPath(base, sizeof(base), tmp, "AC4Coop");
            JoinPath(dir, sizeof(dir), base, "logs");
            DWORD e3 = 0;
            if (CreateDirTree(dir) && ProbeWritable(dir, &e3)) {
                strncpy_s(s_logDir, dir, _TRUNCATE);
                s_logFrom = "temp";
            }
        }
    }

    if (!s_logDir[0]) {
        // Nothing was writable. Still hand out absolute paths next to the ASI (the old behaviour) so the
        // caller's open fails loudly on a known path instead of silently writing into the CWD; logfrom=none
        // says in the diagnostics that this is the degraded case.
        strncpy_s(s_logDir, s_dir, _TRUNCATE);
        s_logFrom = "none";
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

const char* Paths::IniFrom()
{
    EnsureInit();
    return s_iniFrom;
}

const char* Paths::LogDir()
{
    EnsureInit();
    return s_logDir;
}

const char* Paths::LogFrom()
{
    EnsureInit();
    return s_logFrom;
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
            _snprintf_s(e.path, sizeof(e.path), _TRUNCATE, "%s\\%s", s_logDir, leaf);
            out = e.path;
        } else {
            // More distinct leaves than slots: do not alias another entry (that would silently write one
            // log over another). Logs are opened once per process, so this is unreachable in practice.
            static char overflow[MAX_PATH] = { 0 };
            _snprintf_s(overflow, sizeof(overflow), _TRUNCATE, "%s\\%s", s_logDir, leaf);
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

int Paths::Describe(char* dst, size_t cap)
{
    if (!dst || cap == 0) return 0;
    EnsureInit();
    // One line, no embedded newlines: the tester diffs this between runs to see which rule won.
    const int n = _snprintf_s(dst, cap, _TRUNCATE,
                              "[Paths] module=%s ini=%s inifrom=%s logdir=%s logfrom=%s writable=%d err=%lu\r\n",
                              s_dir, s_ini, s_iniFrom, s_logDir, s_logFrom, s_moduleWritable,
                              (unsigned long)s_moduleWriteErr);
    return n > 0 ? n : 0;
}

bool Paths::AppendBeacon(const char* text)
{
    EnsureInit();
    if (!text || !text[0]) return true;

    // Fixed location a tester can always check, even when every log next to the ASI is 0 bytes.
    static char s_beacon[MAX_PATH] = { 0 };
    static bool s_beaconDone = false;
    if (!s_beaconDone) {
        s_beaconDone = true;

        char base[MAX_PATH] = { 0 };
        if (s_localAppData[0]) {
            JoinPath(base, sizeof(base), s_localAppData, "AC4Coop");
        } else {
            char tmp[MAX_PATH] = { 0 };
            const DWORD tn = GetTempPathA((DWORD)MAX_PATH, tmp);
            if (tn > 0 && tn < MAX_PATH && tmp[0]) JoinPath(base, sizeof(base), tmp, "AC4Coop");
        }
        if (base[0]) {
            CreateDirTree(base);
            JoinPath(s_beacon, sizeof(s_beacon), base, "beacon.txt");
        }
    }

    char msg[640] = { 0 };
    if (!s_beacon[0]) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[AC4Coop] beacon path unresolved (no LOCALAPPDATA/TEMP) err=%lu\n",
                    (unsigned long)GetLastError());
        OutputDebugStringA(msg);
        return false;
    }

    // _SH_DENYNO: the beacon is appended from several threads/subsystems; denying writers would make a
    // concurrent append fail where a shared one succeeds.
    FILE* f = _fsopen(s_beacon, "a", _SH_DENYNO);
    if (!f) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[AC4Coop] beacon open FAILED path=%s err=%lu\n",
                    s_beacon, (unsigned long)GetLastError());
        OutputDebugStringA(msg);
        return false;
    }

    const size_t len = strlen(text);
    DWORD err = 0;
    bool ok = fwrite(text, 1, len, f) == len;
    if (!ok) {
        err = GetLastError();
        if (!err) err = ERROR_WRITE_FAULT;
    } else if (fflush(f) != 0) {
        ok = false;
        err = GetLastError();
        if (!err) err = ERROR_WRITE_FAULT;
    }
    fclose(f);

    if (!ok) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[AC4Coop] beacon write FAILED path=%s err=%lu\n",
                    s_beacon, (unsigned long)err);
        OutputDebugStringA(msg);
    }
    return ok;
}

void Paths::NoteOpenFailure(const char* path)
{
    // Read the error FIRST: EnsureInit() below can call into Win32 and clobber it.
    const DWORD err = GetLastError();
    EnsureInit();
    if (!path) path = "";

    EnterCriticalSection(&s_leafLock);
    bool seen = false;
    for (int i = 0; i < s_failedN; ++i) {
        if (strcmp(s_failed[i], path) == 0) { seen = true; break; }
    }
    if (!seen && s_failedN < FAILED_MAX)
        strncpy_s(s_failed[s_failedN++], path, _TRUNCATE);
    LeaveCriticalSection(&s_leafLock);
    if (seen) return;

    char msg[640] = { 0 };
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "[AC4Coop] log open FAILED path=%s err=%lu\n", path, (unsigned long)err);
    OutputDebugStringA(msg);
    AppendBeacon(msg);
}
