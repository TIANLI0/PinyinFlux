#pragma once

#include <string>

namespace pf {

// File logging is off by default (zero I/O on the hot path). Console output is
// used when the process was started with --console.
void LogInit(const std::wstring& path, bool fileEnabled, bool console);
void LogSetFileEnabled(bool enabled);
bool LogFileEnabled();

void Log(const char* fmt, ...);

}  // namespace pf
