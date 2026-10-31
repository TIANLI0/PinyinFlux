#pragma once
#include "common.h"
#include <atomic>
#include <mutex>
#include <string>

namespace pf {
class NativeInjector {
 public:
  ~NativeInjector();
  bool Start(int port);
  void Stop();
  void SetPort(int port) { port_ = port; }
  std::string StatusJson() const;
 private:
  static DWORD WINAPI Thread(void* self);
  void Run();
  HANDLE stop_ = nullptr, thread_ = nullptr;
  std::atomic<int> port_{18765};
  std::atomic<LONG> ready_{0}, pending_{0}, denied_{0}, unsupported_{0}, errors_{0};
  std::atomic<LONG> triggers_{0}, requests_{0};
  std::atomic<LONG> notifications_{0}, lastFlags_{0}, settingsResult_{0};
  std::atomic<DWORD> lastError_{0};
  std::atomic<LONG> diagnostics_[36]{};
  mutable std::mutex layoutMutex_;
  std::string layoutSummary_ = "resolving";
};
}  // namespace pf
