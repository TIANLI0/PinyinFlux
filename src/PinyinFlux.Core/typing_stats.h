#pragma once
#include "common.h"
#include <mutex>
#include <string>

namespace pf {

// Daily typing totals counted by the extension, kept as one line per day in
// %LOCALAPPDATA%\PinyinFlux\stats.csv. Only today's line lives in memory;
// the settings UI reads the file for history.
struct TypingDay {
  uint64_t commits = 0, chars = 0, letters = 0, cloud = 0, activeMs = 0;
};

class TypingStats {
 public:
  TypingStats();
  void Add(const TypingDay& delta);
  // Writes today's line if it changed since the last call.
  void Flush();
  // Forgets everything: today's counts and the file.
  void Clear();
  // ,"typing":{...} for the status endpoint: the day of the latest count, which the UI compares with its own date.
  std::string Json();

  static std::wstring Path();

 private:
  std::mutex mutex_;
  std::string date_;  // yyyy-mm-dd, local time
  TypingDay today_;
  bool dirty_ = false;
};

}  // namespace pf
