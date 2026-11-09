// Crash files and log history, shared by the game and the launcher.
//   crashes/<app>_<time>.dmp   minidump (thread stacks) of an unhandled exception
//   crashes/<app>_<time>.txt   what happened, where, the stack, the version (what the problem report shows)
//   logs/<app>_<time>.log      earlier runs' logs (the current one stays <app>.log)
#pragma once
#include <functional>
#include <string>

#include "common/common.h"

struct _EXCEPTION_POINTERS;

namespace common {

// Writes the crash files for `ep` (null for a fatal stop without an exception, e.g. abort()), flushes the log, and
// returns the summary's path. `reason` says what happened in words.
fs::path WriteCrashReport(const std::string& app, _EXCEPTION_POINTERS* ep, const std::string& reason);
// Unhandled exceptions write the crash files, then `after(summary)` runs (the game opens the launcher's report).
void InstallCrashHandler(const std::string& app, std::function<void(const fs::path& summary)> after = {});
// Moves <name>.log (the previous run's) into logs/<name>_<time>.log, keeping the newest `keep` there.
void RotateLog(const std::string& name, int keep);
// "v0.2.1-3-gabcdef" (git describe at build time), or "dev".
const char* Version();

} // namespace common
