// SPDX-License-Identifier: MIT
// Whether the extension may enter the live input host on this Windows build.
//
// A Windows update can change Microsoft Pinyin in ways the symbol-based layout
// does not notice. Before the first injection into a build (or with a new
// extension), the core therefore installs the extension into a disposable
// child process holding the same system images, and remembers the verdict per
// images + extension. After injecting, a host that crashes soon after counts
// against the build; twice and it is no longer injected.
#pragma once
#include <map>
#include <mutex>
#include <string>

#include "../PinyinFlux.Inject/protocol.h"

namespace pf {

// Child process side (PinyinFluxCore.exe --preflight <extension>): loads the
// installed Microsoft Pinyin images into this process without running them,
// installs the extension exactly as in the input host, sends a cloud request
// through the redirected WinHTTP imports to a loopback listener, and stops it
// again. Prints one line, "PASS ..." or "FAIL <step>: <detail>"; exits 0 on success.
int RunPreflight(const std::wstring& extension);

// Parent side: runs the check in a child process for at most 20 seconds. A
// crash or hang of the child is a failure. `detail` receives its line.
bool Preflight(const std::wstring& extension, std::string& detail);

// Names the system images and extension a verdict applies to.
std::string CompatKey(const native::NativeLayout& layout, const std::wstring& extension);

// %LOCALAPPDATA%\PinyinFlux\compat.ini: one line per checked combination.
class CompatStore {
 public:
  enum Verdict { Unknown, Pass, Fail, Crashed };
  struct Entry {
    Verdict verdict = Unknown;
    int crashes = 0;  // host crashes shortly after injecting
    std::string detail;
  };
  // An empty path keeps the verdicts in memory.
  explicit CompatStore(std::wstring path);
  static std::wstring DefaultPath();
  Entry Get(const std::string& key) const;
  void Set(const std::string& key, const Entry& entry);
  // Counts a host crash; returns the count for this combination.
  int Crash(const std::string& key);

 private:
  void Save() const;
  std::wstring path_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
};

}  // namespace pf
