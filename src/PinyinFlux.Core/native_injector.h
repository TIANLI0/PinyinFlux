#pragma once
#include "common.h"
#include "../PinyinFlux.Inject/protocol.h"
#include "ranking.h"
#include "typing_stats.h"
#include <atomic>
#include <memory>
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
  void ClearTypingStats() { typing_.Clear(); }
  // Receives the cloud choices reported by the extension; set before Start.
  void SetRankModel(std::shared_ptr<RankModel> model) { ranking_ = std::move(model); }
 private:
  static DWORD WINAPI Thread(void* self);
  void Run();
  // Whether this Windows build may be injected (see native_compat.h):
  // resolving, waitingSymbols, checking, verified, unsupported, crashed or error.
  void SetCompat(const char* state, const std::string& detail);
  HANDLE stop_ = nullptr, thread_ = nullptr;
  std::atomic<int> port_{18765};
  std::atomic<LONG> ready_{0}, pending_{0}, denied_{0}, unsupported_{0}, errors_{0}, restartPending_{0}, doublePinyin_{0};
  std::atomic<LONG> triggers_{0}, requests_{0};
  std::atomic<LONG> notifications_{0}, lastFlags_{0}, settingsResult_{0};
  std::atomic<DWORD> lastError_{0};
  std::atomic<LONG> diagnostics_[36]{};
  mutable std::mutex layoutMutex_;  // guards the strings and timing below
  std::string layoutSummary_ = "resolving";
  std::string compatState_ = "resolving", compatDetail_, windowsVersion_;
  struct Timing {
    LONG count = 0, sum[3]{}, buckets[native::kTimedBuckets]{}, cloudDelay = -1, missed = 0;
  } timing_;
  mutable TypingStats typing_;
  std::shared_ptr<RankModel> ranking_;
};
}  // namespace pf
