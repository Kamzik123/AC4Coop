// Build gate implementation -- see build_gate.h.
//
// SHA-256 of the running EXE via CNG/BCrypt (bcrypt.lib), streamed in 1 MB chunks so a 45 MB image is
// never held in memory. No shipped DLL is added: BCrypt is a Win7+ system library.
//
// The known-build list is OUTSIDE the binary, next to the ASI:
//
//   <dir of AC4Coop.asi>\known_builds.txt
//
// Format, one entry per line:   <64-hex-sha256> <label>
// `#` comments and blank lines are ignored. A missing or unreadable list means "no known builds", i.e.
// the running EXE is treated as UNKNOWN_BUILD (logged as such) -- never as trusted.
//
// One record per process start is written to three places: OutputDebugStringA, AC4Coop_build_<pid>.log
// and build-provenance.txt (append-only), in exactly this shape:
//
//   [BG] ts=<local ISO8601 ms> pid=<pid> path=<...> size=<n> mtime=<filetime> sha256=<hex> imagebase=<0x...> verdict=KNOWN <label>
//   [BG] ts=<local ISO8601 ms> pid=<pid> path=<...> size=<n> mtime=<filetime> sha256=<hex> imagebase=<0x...> verdict=UNKNOWN_BUILD
//   (one CRLF-terminated line per record, as above; the two shapes differ only in the tail)
//
// ts= and pid= come first and the original fields keep their order, so existing grep patterns still
// match while the record is now orderable and attributable.

#include "build_gate.h"
#include "paths.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>

#pragma comment(lib, "bcrypt.lib")

namespace {

const DWORD  CHUNK_SIZE        = 1024u * 1024u;   // HashData() call size
// ini path: Paths::Ini() (module-relative; see src/paths.h)
const char*  KNOWN_BUILDS_FILE = "known_builds.txt";

// Append-only provenance trail, in the resolved log directory (Paths::LogDir(); may be
// %LOCALAPPDATA%\AC4Coop\logs when the module directory itself is not writable).
const char* ProvPath()
{
    return Paths::LogPath("build-provenance.txt");
}

// This unit's log is per-PID: a process that never exited (a zombie holding AC4Coop_build.log with
// _SH_DENYWR) must not be able to block a later run from writing its own [BG] record.
const char* BuildLogPath()
{
    static char leaf[64] = { 0 };
    if (!leaf[0]) {
        _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "AC4Coop_build_%u.log",
                    (unsigned)GetCurrentProcessId());
    }
    return Paths::LogPath(leaf);
}

// Local wall-clock ISO8601 with milliseconds. Provenance records carried neither a time nor a PID, which
// left 8 records across 3 launches unorderable; ts= and pid= are written into every record now.
void LocalTimestamp(char* out, size_t cap)
{
    SYSTEMTIME st = { 0 };
    GetLocalTime(&st);
    _snprintf_s(out, cap, _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
                (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                (unsigned)st.wMilliseconds);
}

bool  g_verified = false;   // Verify() ran in this process
bool  g_trusted  = false;   // ... and its verdict
bool  g_known    = false;   // ... because the digest was listed
char  g_sha256[65] = { 0 };
char  g_label[160] = { 0 };

FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    OutputDebugStringA(buf);
    if (!g_log) g_log = _fsopen(BuildLogPath(), "a", _SH_DENYWR);
    if (!g_log) Paths::NoteOpenFailure(BuildLogPath());
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

// Append one record to `path` as a single WriteFile of a CRLF-terminated line.
//
// CreateFileW with FILE_APPEND_DATA and FILE_SHARE_READ|FILE_SHARE_WRITE -- not fopen/_fsopen -- because
// the failure that lost records was an exclusive CRT handle in another live process: a shared append
// handle cannot be blocked by that, and two live processes can append concurrently without interleaving
// (the append flag makes each write land at EOF atomically).
bool AppendRaw(const char* path, const char* line, size_t len)
{
    wchar_t wide[MAX_PATH * 2] = { 0 };
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide,
                             (int)(sizeof(wide) / sizeof(wide[0]))))
        return false;

    HANDLE h = CreateFileW(wide, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("[BG] provenance open failed (%lu) %s\n", (unsigned long)GetLastError(), path);
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(h, line, (DWORD)len, &written, nullptr);
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);

    if (!ok || written != (DWORD)len) {
        Log("[BG] provenance write failed (%lu) %s (%lu/%lu bytes)\n",
            (unsigned long)GetLastError(), path, (unsigned long)written, (unsigned long)len);
        return false;
    }
    return true;
}

// Append-only provenance trail: one line per process start, never rotated or truncated.
// A record must never be silently dropped: if the shared file cannot be appended to, the same line goes
// to a per-PID fallback next to it and the fallback is announced on OutputDebugString.
void AppendProvenance(const char* line)
{
    // Exactly one CRLF-terminated line, whatever the caller passed.
    char buf[1200];
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.*s\r\n", (int)n, line);
    const size_t len = strlen(buf);

    if (AppendRaw(ProvPath(), buf, len)) return;

    char leaf[96] = { 0 };
    _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "build-provenance-fallback_%u.txt",
                (unsigned)GetCurrentProcessId());
    char fallback[MAX_PATH] = { 0 };
    Paths::LogPath(fallback, sizeof(fallback), leaf);

    char msg[320];
    if (AppendRaw(fallback, buf, len)) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[BG] provenance PRIMARY FAILED -> record appended to %s instead\n", fallback);
    } else {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[BG] provenance LOST: primary and %s both failed\n", fallback);
    }
    OutputDebugStringA(msg);
    Log("%s", msg);
}

void WriteRecord(const char* line)
{
    OutputDebugStringA(line);
    if (!g_log) g_log = _fsopen(BuildLogPath(), "a", _SH_DENYWR);
    if (!g_log) Paths::NoteOpenFailure(BuildLogPath());
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    AppendProvenance(line);
}

void ToHex(const BYTE* digest, DWORD n, char* out)
{
    static const char kHex[] = "0123456789abcdef";
    for (DWORD i = 0; i < n; ++i) {
        out[i * 2 + 0] = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    out[n * 2] = 0;
}

struct FileFacts {
    ULONGLONG size;
    ULONGLONG mtime;
};

// SHA-256 of `path`. Opened with full sharing so the game's own open handle cannot block us, and so
// we never take a lock the running process needs. Returns false on any failure (missing, locked,
// permission, hash error) -- the caller treats that as an unknown build.
bool Sha256File(const wchar_t* path, char hex[65], FileFacts* facts)
{
    hex[0] = 0;
    if (facts) { facts->size = 0; facts->mtime = 0; }

    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log("[BG] CreateFileW(%ls) failed (%lu)\n", path, (unsigned long)GetLastError());
        return false;
    }

    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);

    FILETIME ft = {};
    GetFileTime(file, nullptr, nullptr, &ft);
    if (facts) {
        facts->size  = (ULONGLONG)size.QuadPart;
        facts->mtime = ((ULONGLONG)ft.dwHighDateTime << 32) | (ULONGLONG)ft.dwLowDateTime;
    }

    if (size.QuadPart == 0) {
        Log("[BG] %ls is empty -- refusing to treat 0 bytes as a build\n", path);
        CloseHandle(file);
        return false;
    }

    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    PUCHAR obj = nullptr;
    PUCHAR buf = nullptr;
    bool   ok  = false;

    do {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            Log("[BG] BCryptOpenAlgorithmProvider(SHA256) failed\n");
            break;
        }

        DWORD objLen = 0, written = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen,
                                              sizeof(objLen), &written, 0))) {
            Log("[BG] BCryptGetProperty(OBJECT_LENGTH) failed\n");
            break;
        }

        obj = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, objLen);
        buf = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, CHUNK_SIZE);
        if (!obj || !buf) {
            Log("[BG] out of memory allocating hash state\n");
            break;
        }

        if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, obj, objLen, nullptr, 0, 0))) {
            Log("[BG] BCryptCreateHash failed\n");
            break;
        }

        bool readOk = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(file, buf, CHUNK_SIZE, &got, nullptr)) {
                Log("[BG] ReadFile failed (%lu)\n", (unsigned long)GetLastError());
                readOk = false;
                break;
            }
            if (got == 0) break;   // EOF
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, buf, got, 0))) {
                Log("[BG] BCryptHashData failed\n");
                readOk = false;
                break;
            }
        }
        if (!readOk) break;

        BYTE digest[32] = { 0 };
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, (ULONG)sizeof(digest), 0))) {
            Log("[BG] BCryptFinishHash failed\n");
            break;
        }
        ToHex(digest, (DWORD)sizeof(digest), hex);
        ok = true;
    } while (0);

    if (hash) BCryptDestroyHash(hash);
    if (obj)  HeapFree(GetProcessHeap(), 0, obj);
    if (buf)  HeapFree(GetProcessHeap(), 0, buf);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);

    return ok;
}

// Directory that contains this ASI, from the handle the loader gave us (never a hardcoded path).
bool ModuleDirectoryA(HMODULE self, char* out, size_t cap)
{
    char full[MAX_PATH * 2] = { 0 };
    DWORD n = GetModuleFileNameA(self, full, (DWORD)(sizeof(full) / sizeof(full[0])));
    if (n == 0 || n >= sizeof(full) / sizeof(full[0])) return false;

    char* slash = strrchr(full, '\\');
    if (!slash) return false;
    *slash = 0;

    _snprintf_s(out, cap, _TRUNCATE, "%s", full);
    return true;
}

bool IsSpaceA(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// `<64-hex-sha256> <label>` per line; `#` comments and blank lines ignored.
// Returns true when the list was readable AND contains `shaHex` (case-insensitive).
bool LookupKnownBuild(const char* dir, const char* shaHex,
                      bool* listFound, char* labelOut, size_t labelCap)
{
    if (listFound) *listFound = false;
    labelOut[0] = 0;

    char path[MAX_PATH * 3] = { 0 };
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", dir, KNOWN_BUILDS_FILE);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;
    if (listFound) *listFound = true;

    char line[512];
    bool found = false;
    while (fgets(line, (int)sizeof(line), f)) {
        char* p = line;
        while (*p && IsSpaceA(*p)) ++p;
        if (*p == 0 || *p == '#') continue;

        // First token: exactly 64 hex characters.
        size_t n = 0;
        while (p[n] && !IsSpaceA(p[n])) ++n;
        if (n != 64) {
            Log("[BG] known_builds.txt: ignoring malformed entry (token length %u)\n", (unsigned)n);
            continue;
        }

        char tok[65];
        memcpy(tok, p, 64);
        tok[64] = 0;
        if (_stricmp(tok, shaHex) != 0) continue;

        char* label = p + n;
        while (*label && IsSpaceA(*label)) ++label;
        size_t L = strlen(label);
        while (L && IsSpaceA(label[L - 1])) label[--L] = 0;

        _snprintf_s(labelOut, labelCap, _TRUNCATE, "%s", L ? label : "(unlabelled)");
        found = true;
        break;
    }

    fclose(f);
    return found;
}

} // namespace

bool BuildGate::Verify(HMODULE self)
{
    if (g_verified) return g_trusted;   // one record per process start
    g_verified = true;
    g_trusted  = false;
    g_known    = false;

    // ---- 1. the EXE that is actually running ------------------------------------------------
    wchar_t exePath[MAX_PATH * 2] = { 0 };
    const DWORD len = GetModuleFileNameW(nullptr, exePath,
                                         (DWORD)(sizeof(exePath) / sizeof(exePath[0])));
    const bool havePath = (len > 0) && (len < sizeof(exePath) / sizeof(exePath[0]));
    if (!havePath)
        Log("[BG] GetModuleFileNameW(NULL) failed (%lu) -- build unknown\n",
            (unsigned long)GetLastError());

    // ---- 2. digest --------------------------------------------------------------------------
    FileFacts facts = { 0, 0 };
    char hex[65] = { 0 };
    bool hashed = false;
    if (havePath)
        hashed = Sha256File(exePath, hex, &facts);   // logs its own failure detail
    if (hashed)
        memcpy(g_sha256, hex, sizeof(g_sha256));

    // ---- 3. known-build lookup, next to this ASI --------------------------------------------
    bool listFound = false;
    if (hashed) {
        char dir[MAX_PATH * 2] = { 0 };
        if (!ModuleDirectoryA(self, dir, sizeof(dir))) {
            Log("[BG] cannot resolve the ASI directory (%lu) -- known_builds.txt unavailable\n",
                (unsigned long)GetLastError());
        } else {
            g_known = LookupKnownBuild(dir, g_sha256, &listFound, g_label, sizeof(g_label));
            if (!listFound)
                Log("[BG] known_builds.txt MISSING in %s -- build unknown\n", dir);
        }
    } else {
        Log("[BG] no digest -- known_builds.txt not consulted, build unknown\n");
    }

    // ---- 4. the one-line record -------------------------------------------------------------
    const uintptr_t imageBase = (uintptr_t)GetModuleHandleW(nullptr);
    const char* shaText = hashed ? g_sha256 : "(hash-failed)";

    char record[1200];
    char ts[32];
    LocalTimestamp(ts, sizeof(ts));
    if (g_known) {
        _snprintf_s(record, sizeof(record), _TRUNCATE,
                    "[BG] ts=%s pid=%u path=%ls size=%I64u mtime=%I64u sha256=%s "
                    "imagebase=0x%08X verdict=KNOWN %s\n",
                    ts, (unsigned)GetCurrentProcessId(), exePath, facts.size, facts.mtime, shaText,
                    (unsigned)imageBase, g_label);
    } else {
        _snprintf_s(record, sizeof(record), _TRUNCATE,
                    "[BG] ts=%s pid=%u path=%ls size=%I64u mtime=%I64u sha256=%s "
                    "imagebase=0x%08X verdict=UNKNOWN_BUILD\n",
                    ts, (unsigned)GetCurrentProcessId(), exePath, facts.size, facts.mtime, shaText,
                    (unsigned)imageBase);
    }
    WriteRecord(record);

    // ---- 5. verdict -------------------------------------------------------------------------
    const int force = GetPrivateProfileIntA("Coop", "ForceUnknownBuild", 0, Paths::Ini());

    if (g_known) {
        g_trusted = true;
        Log("[BG] build trusted: %s\n", g_label);
    } else if (force) {
        // Loud on purpose: this is the one path where unverified code gets patched.
        for (int i = 0; i < 3; ++i) {
            Log("[BG] ***************************************************************\n");
            Log("[BG] WARNING: [Coop] ForceUnknownBuild=1\n");
            Log("[BG] WARNING: UNKNOWN BUILD TRUSTED FOR THIS SESSION ONLY\n");
            Log("[BG] WARNING: patches are being installed against unverified code\n");
        }
        Log("[BG] ***************************************************************\n");
        g_trusted = true;
    } else {
        g_trusted = false;
        Log("[BG] build NOT trusted -- module stays resident, no patches installed\n");
    }

    return g_trusted;
}

bool BuildGate::Trusted()
{
    return g_trusted;
}
