#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace pf::native {
inline constexpr DWORD kMagic = 0x50464e32;
enum State : LONG { Waiting = 0, Active = 1, Unsupported = 2, Stopped = 3, Failed = 4 };

// Version-specific addresses, resolved by the core from Microsoft's public
// symbols for the images installed on this machine (see native_symbols.cpp).
// An rva of 0 means unavailable; the extension then skips that hook.
struct ImageId {
  DWORD stamp = 0, size = 0;  // TimeDateStamp and SizeOfImage
  bool operator==(const ImageId&) const = default;
};
inline constexpr size_t kEntryBytes = 16;
struct Function {
  DWORD rva = 0;
  BYTE entry[kEntryBytes]{};  // first bytes on disk: the loaded code must match
};
// InputService.dll (CHS conversion engine hosted by ctfmon).
struct EngineLayout {
  ImageId image;
  // Required for native cloud candidates.
  Function pinyinChanged;    // ChxIME::CandidateHandlerConversionPinyin::OnProductItemChanged
  Function triggerRule;      // ChxIME::CandidateHandlerConversionPinyin::CheckCOCTriggerRule
  Function preloading;       // ChxIME::CandidateHandlerConversionPinyin::CreateCOCPreloadingSuggestionList
  Function setExcerpts;      // ChxIME::ProductItemPool::SetCOCExcerptResult
  Function desktopChanged;   // ChxIME::CandidateHandlerConversionOfCloud::OnProductItemChanged
  DWORD listClass = 0, listInterface = 0;  // CLSID/IID_MtfSuggestionList
  DWORD ruleFlags[3]{};      // trigger rule object: consent and two profile bytes
  DWORD ruleSettings = 0;    // trigger rule object: settings interface
  DWORD rulePool = 0;        // trigger rule object: product item pool
  DWORD settingsSlots[4]{};  // settings vtable byte offsets: scope, enabled, two S_FALSE checks
  DWORD poolSpelling = 0;    // pool vtable byte offset: GetStringRef(id, text, length)
  DWORD excerpts = 0;        // product item pool: stored cloud excerpts vector
  DWORD candidateText = 0;   // local candidate info record: text offset field (0 = unknown)
  // Optional.
  Function cloudChanged;     // ChsIME::CandidateHandlerConversionOfCloud::OnProductItemChanged
  Function settingRule;      // ChsIME::CandidateHandlerConversionOfCloud::CheckCOCTriggerSettingRule
  Function candidateRule;    // ChsIME::CandidateHandlerConversionOfCloud::CheckCOCTriggerCandidateRule
  DWORD cloudBase = 0;       // cloudChanged this -> rule object
  Function doCloudQuery, requestSuggestion, suggestionReady, filterSuggestions, spellQuery, displayOrder;
  // Typing statistics (optional): receives every committed conversion and its reading.
  Function commitText;       // ChxIME::ProductItemPool::SetPrevPrimitive
};
// Legacy ChsEM.dll (ChsIME.exe host): only the cloud handler is patched.
struct LegacyLayout {
  ImageId image;
  Function cloudChanged;     // ChxIME::CandidateHandlerConversionOfCloud::OnProductItemChanged
};
// ChsProxyDS.dll (cloud request module). WinHTTP redirection works on any
// version; these hooks refine it.
struct ProxyLayout {
  ImageId image;
  Function getCandidates;    // ChsQueryHandler::GetCandidates
  Function localCache;       // ChsQueryHandler::GetCandidatesFromLocalCache
  Function addCloud;         // ChsQueryHandler::AddCloudCandidateData
  Function addResponse;      // ChsQueryHandler::AddResponseInfor
  Function debounce;         // ChsQueryHandler::IsExpiredDuringCloudDelay
  DWORD requestLock = 0;     // ChsQueryHandler::s_fCloudTriggerring
  DWORD cloudDelay = 0;      // ChsQueryHandler field: typing pause before a request, ms (0 = unknown)
};
struct NativeLayout {
  EngineLayout engine;
  LegacyLayout legacy;
  ProxyLayout proxy;
};

// Names a native cloud answer by its words (FNV-1a over UTF-16, each word
// terminated), so a choice reported back to the core carries no text.
inline DWORD AnswerHash(const std::vector<std::wstring>& words) {
  DWORD hash = 2166136261u;
  for (std::wstring_view word : words) {
    for (wchar_t c : word) hash = (hash ^ static_cast<DWORD>(c)) * 16777619u;
    hash *= 16777619u;
  }
  return hash;
}
// One commit while cloud words were shown, for adaptive ranking.
struct Choice {
  LONG answer = 0;  // AnswerHash of the response
  LONG shown = 0;   // bit i: response word i was displayed
  LONG picked = -1; // index of the committed response word, -1 for another word
};
inline constexpr LONG kChoices = 16;
inline constexpr int kTimedBuckets = 5;
inline constexpr LONG kTimedLimits[kTimedBuckets - 1] = {300, 600, 1000, 2000};
// What the user saw of a cloud answer when `committed` was committed. The
// native filter drops response words that are local candidates; Microsoft
// Pinyin then displays as many of the rest as SetCloudExcerpts stored
// (`displayed`), in order: one on the builds verified so far, though the
// filter keeps up to three. Words it kept back were never seen, so they must
// not count as ignored.
inline Choice ChoiceFor(const std::vector<std::wstring>& response, const std::vector<std::wstring>& local,
                        LONG displayed, std::wstring_view committed) {
  Choice choice;
  choice.answer = static_cast<LONG>(AnswerHash(response));
  for (size_t i = 0; displayed > 0 && i < response.size() && i < 31; ++i) {
    if (std::find(local.begin(), local.end(), response[i]) != local.end()) continue;
    choice.shown |= 1L << i;
    --displayed;
    if (!committed.empty() && response[i] == committed) choice.picked = static_cast<LONG>(i);
  }
  return choice;
}

struct Channel {
  DWORD magic = kMagic;
  DWORD port = 18765;
  volatile LONG enabled = 1;
  volatile LONG heartbeat = 0;
  volatile LONG state = Waiting;
  volatile LONG error = 0;
  volatile LONG triggerSlots = 0;
  volatile LONG networkSlots = 0;
  volatile LONG triggers = 0;
  volatile LONG requests = 0;
  volatile LONG notifications = 0;
  volatile LONG lastFlags = 0;
  volatile LONG settingsResult = 0;
  volatile LONG diagnostics[36]{};
  // Typing statistics, counted by the extension while statsEnabled; never the text itself.
  volatile LONG statsEnabled = 0;  // written by the core from its config
  volatile LONG statCommits = 0;   // committed conversions
  volatile LONG statChars = 0;     // Chinese characters in them
  volatile LONG statLetters = 0;   // pinyin letters typed for them
  volatile LONG statCloud = 0;     // conversions that were a PinyinFlux cloud word
  volatile LONG statActiveMs = 0;  // typing time, pauses excluded
  volatile LONG statShown = 0;     // spellings for which a cloud word was displayed
  // Adaptive ranking: the extension writes choices[n % kChoices], then raises
  // choiceCount to n + 1; the core reads what is new since its last pass.
  volatile LONG choiceCount = 0;
  Choice choices[kChoices]{};
  // How long a cloud word takes to appear, over the displays timed so far (ms):
  // timedSum[0] last keystroke -> request (Microsoft's typing-pause wait),
  // [1] request -> first answer (the core and the network), [2] first answer
  // -> displayed (the replay). timedBuckets: totals < 300, 600, 1000, 2000, more.
  volatile LONG timedCount = 0;
  volatile LONG timedSum[3]{};
  volatile LONG timedBuckets[kTimedBuckets]{};
  volatile LONG cloudDelay = -1;   // ChsProxyDS's typing-pause wait as configured, ms (-1 unknown)
  volatile LONG timedMissed = 0;   // commits while our cloud request was still unanswered
  volatile LONG doublePinyin = 0;  // Microsoft Pinyin is set to 双拼: no cloud words from us
  NativeLayout layout;  // written by the core before the extension initializes
};
inline std::wstring MappingName(DWORD process) {
  return L"Local\\PinyinFlux.Native.v8." + std::to_wstring(process);
}
inline bool Enabled(Channel* c) {
  return c && c->magic == kMagic && InterlockedCompareExchange(&c->enabled, 0, 0) &&
      static_cast<DWORD>(GetTickCount() - static_cast<DWORD>(InterlockedCompareExchange(&c->heartbeat, 0, 0))) < 5000;
}
}  // namespace pf::native
