#pragma once

#include <cstddef>

// Resolution of the ini and every log file, anchored to the directory that holds this ASI -- NOT to the
// process working directory. Under Ubisoft Connect the CWD is the launcher folder, so every relative
// ".\\AC4Coop.ini" missed, [Net] Enabled fell back to 0 and the socket was never created. Everything that
// reads the ini or opens a log therefore goes through this unit.
//
// 2026-10-29 hardening: under "C:\Program Files (x86)" the ASI loads but the process cannot write next to
// it, so every log file was created 0 bytes. The log directory is therefore resolved ONCE by probing:
//   module dir (only if a real write test succeeds) -> %LOCALAPPDATA%\AC4Coop\logs -> %TEMP%\AC4Coop\logs.
// The decision, including the module-dir write error, is reported by Describe() and mirrored to a fixed
// beacon file so a tester never has to guess where the diagnostics went.
//
// All returned pointers have process lifetime (static storage); callers must not free or keep them across
// a theoretical reload.
namespace Paths {

// Absolute directory of the module that owns this code (no trailing backslash). Cached on first call.
const char* ModuleDir();

// Absolute ini path, resolved once: "<ModuleDir>\\AC4Coop.ini" when that file exists, else
// "%LOCALAPPDATA%\\AC4Coop\\AC4Coop.ini" when that one exists, else the legacy relative
// ".\\AC4Coop.ini" (so a setup that relied on the CWD keeps working).
const char* Ini();

// 1 when Ini() resolved next to the ASI, 0 otherwise (localappdata or CWD fallback in use).
int IniAtModule();

// "module" | "localappdata" | "cwd" | "none" -- which ini() rule won.
const char* IniFrom();

// The directory every log file is written to (no trailing backslash), resolved once. Never empty: when
// no candidate is writable it degrades to the module directory and LogFrom() reports "none".
const char* LogDir();

// "module" | "localappdata" | "temp" | "none".
const char* LogFrom();

// Absolute "<LogDir>\\<leaf>". Cached per distinct leaf string (pointers stay valid; the leaf buffer
// itself is copied).
const char* LogPath(const char* leaf);
void        LogPath(char* dst, size_t cap, const char* leaf);

// The one-line startup answer, exactly:
//   [Paths] module=%s ini=%s inifrom=%s logdir=%s logfrom=%s writable=%d err=%lu\r\n
// `writable`/`err` are the module-directory write probe: 1/0 and the Win32 error of a failed probe (0 if
// the probe succeeded). Returns the number of bytes written (0 on failure/truncation). `cap` >= 1024.
int Describe(char* dst, size_t cap);

// Append `text` verbatim to "%LOCALAPPDATA%\\AC4Coop\\beacon.txt" (directories created, "a", flushed and
// closed on every call so nothing is buffered). On any failure the reason plus GetLastError() goes to
// OutputDebugStringA. Returns true when every write call succeeded.
bool AppendBeacon(const char* text);

// Report a failed fopen/_fsopen of a log file once per distinct path: emits
//   [AC4Coop] log open FAILED path=%s err=%lu\n
// (GetLastError() read on entry) to OutputDebugStringA and to the beacon. Call immediately after the
// failing open so the error still belongs to it.
void NoteOpenFailure(const char* path);

} // namespace Paths
