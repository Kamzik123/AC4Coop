#pragma once

#include <cstddef>

// Resolution of the ini and every log file, anchored to the directory that holds this ASI -- NOT to the
// process working directory. Under Ubisoft Connect the CWD is the launcher folder, so every relative
// ".\\AC4Coop.ini" missed, [Net] Enabled fell back to 0 and the socket was never created. Everything that
// reads the ini or opens a log therefore goes through this unit.
//
// All returned pointers have process lifetime (static storage); callers must not free or keep them across
// a theoretical reload.
namespace Paths {

// Absolute directory of the module that owns this code (no trailing backslash). Cached on first call.
const char* ModuleDir();

// Absolute "<ModuleDir>\\AC4Coop.ini" when that file exists; otherwise the legacy relative
// ".\\AC4Coop.ini" (so a setup that relied on the CWD keeps working). Cached: the decision is made once.
const char* Ini();

// 1 when Ini() resolved to the module directory, 0 when the CWD fallback is in use.
int IniAtModule();

// Absolute "<ModuleDir>\\<leaf>". Never falls back: a log must not land somewhere else by accident.
// Cached per distinct leaf string (pointers stay valid; the leaf buffer itself is copied).
const char* LogPath(const char* leaf);
void        LogPath(char* dst, size_t cap, const char* leaf);

// The one-line startup answer, "[Paths] module=<dir> ini=<abs path> exists=<0|1> logdir=<dir>" plus CRLF.
// Returns the number of bytes written (0 on failure/truncation). `cap` should be at least 1024.
int FormatStartupLine(char* dst, size_t cap);

} // namespace Paths
