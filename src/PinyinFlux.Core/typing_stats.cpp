// SPDX-License-Identifier: MIT
#include "typing_stats.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace pf {
namespace {
std::string Today() {
  SYSTEMTIME t{};
  GetLocalTime(&t);
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
  return buffer;
}

// date,commits,chars,letters,cloud,activeSeconds
std::string Line(const std::string& date, const TypingDay& d) {
  return date + ',' + std::to_string(d.commits) + ',' + std::to_string(d.chars) + ',' + std::to_string(d.letters) + ',' +
         std::to_string(d.cloud) + ',' + std::to_string(d.activeMs / 1000);
}

std::vector<std::string> ReadLines() {
  std::vector<std::string> lines;
  std::ifstream in(TypingStats::Path());
  for (std::string line; std::getline(in, line);)
    if (!line.empty() && line[0] != '#') lines.push_back(line);
  return lines;
}

TypingDay ReadDay(const std::string& date) {
  TypingDay d;
  for (const auto& line : ReadLines()) {
    if (line.compare(0, date.size() + 1, date + ',') != 0) continue;
    std::istringstream fields(line.substr(date.size() + 1));
    char comma = 0;
    uint64_t seconds = 0;
    fields >> d.commits >> comma >> d.chars >> comma >> d.letters >> comma >> d.cloud >> comma >> seconds;
    d.activeMs = seconds * 1000;
  }
  return d;
}
}  // namespace

std::wstring TypingStats::Path() { return DataDir() + L"\\stats.csv"; }

// Replaces (or appends) one day's line. Written to a sibling and swapped in, so a crash never leaves half a file.
static void WriteDay(const std::string& date, const TypingDay& day) {
  std::vector<std::string> lines = ReadLines();
  std::string line = Line(date, day);
  bool replaced = false;
  for (auto& existing : lines)
    if (existing.compare(0, date.size() + 1, date + ',') == 0) { existing = line; replaced = true; }
  if (!replaced) lines.push_back(line);
  std::wstring path = TypingStats::Path(), temp = path + L".tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    out << "# date,commits,chars,letters,cloud,activeSeconds\n";
    for (const auto& l : lines) out << l << '\n';
    if (!out) return;
  }
  MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

TypingStats::TypingStats() : date_(Today()), today_(ReadDay(date_)) {}

void TypingStats::Add(const TypingDay& delta) {
  if (!delta.commits && !delta.activeMs) return;
  std::string finishedDate;
  TypingDay finished;
  {
    std::lock_guard lock(mutex_);
    // Past local midnight: yesterday's unsaved counts are written out, today starts from the file.
    if (std::string now = Today(); now != date_) {
      if (dirty_) { finishedDate = date_; finished = today_; }
      date_ = now;
      today_ = ReadDay(now);
    }
    today_.commits += delta.commits;
    today_.chars += delta.chars;
    today_.letters += delta.letters;
    today_.cloud += delta.cloud;
    today_.activeMs += delta.activeMs;
    dirty_ = true;
  }
  if (!finishedDate.empty()) WriteDay(finishedDate, finished);
}

void TypingStats::Flush() {
  std::string date;
  TypingDay day;
  {
    std::lock_guard lock(mutex_);
    if (!dirty_) return;
    dirty_ = false;
    date = date_;
    day = today_;
  }
  WriteDay(date, day);
}

void TypingStats::Clear() {
  std::lock_guard lock(mutex_);
  date_ = Today();
  today_ = {};
  dirty_ = false;
  DeleteFileW(Path().c_str());
}

std::string TypingStats::Json() {
  std::lock_guard lock(mutex_);
  return ",\"typing\":{\"date\":\"" + date_ + "\",\"commits\":" + std::to_string(today_.commits) +
         ",\"chars\":" + std::to_string(today_.chars) + ",\"letters\":" + std::to_string(today_.letters) +
         ",\"cloud\":" + std::to_string(today_.cloud) + ",\"activeSeconds\":" + std::to_string(today_.activeMs / 1000) + "}";
}

}  // namespace pf
