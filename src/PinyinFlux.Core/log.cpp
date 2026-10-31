#include "log.h"

#include <share.h>

#include <cstdarg>
#include <cstdio>

#include "common.h"

namespace pf {

namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_path;
FILE* g_file = nullptr;
bool g_fileEnabled = false;
bool g_console = false;
constexpr long kMaxLogBytes = 2 * 1024 * 1024;

void OpenFileLocked() {
  if (g_file || g_path.empty()) return;
  // Keep the log bounded: start over when it grows past the cap.
  WIN32_FILE_ATTRIBUTE_DATA attr{};
  bool truncate = GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &attr) &&
                  attr.nFileSizeLow > static_cast<DWORD>(kMaxLogBytes);
  // Shared so the log can be read (and tailed) while the core is running.
  g_file = _wfsopen(g_path.c_str(), truncate ? L"wb" : L"ab", _SH_DENYNO);
}
}  // namespace

void LogInit(const std::wstring& path, bool fileEnabled, bool console) {
  AcquireSRWLockExclusive(&g_lock);
  g_path = path;
  g_fileEnabled = fileEnabled;
  g_console = console;
  ReleaseSRWLockExclusive(&g_lock);
}

void LogSetFileEnabled(bool enabled) {
  AcquireSRWLockExclusive(&g_lock);
  g_fileEnabled = enabled;
  if (!enabled && g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
  ReleaseSRWLockExclusive(&g_lock);
}

bool LogFileEnabled() { return g_fileEnabled; }

void Log(const char* fmt, ...) {
  if (!g_fileEnabled && !g_console) return;

  char msg[2048];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[2200];
  int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%5lu] %s\n", st.wHour, st.wMinute, st.wSecond,
                   st.wMilliseconds, GetCurrentThreadId(), msg);
  if (n < 0) return;
  if (n >= static_cast<int>(sizeof(line))) n = static_cast<int>(sizeof(line)) - 1;

  AcquireSRWLockExclusive(&g_lock);
  if (g_console) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, n, &written, nullptr);
  }
  if (g_fileEnabled) {
    OpenFileLocked();
    if (g_file) {
      fwrite(line, 1, n, g_file);
      fflush(g_file);
    }
  }
  ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace pf
