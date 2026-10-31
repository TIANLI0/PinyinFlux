#pragma once
#include <windows.h>
#include <string>

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
};
struct NativeLayout {
  EngineLayout engine;
  LegacyLayout legacy;
  ProxyLayout proxy;
};

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
  NativeLayout layout;  // written by the core before the extension initializes
};
inline std::wstring MappingName(DWORD process) {
  return L"Local\\PinyinFlux.Native.v5." + std::to_wstring(process);
}
inline bool Enabled(Channel* c) {
  return c && c->magic == kMagic && InterlockedCompareExchange(&c->enabled, 0, 0) &&
      static_cast<DWORD>(GetTickCount() - static_cast<DWORD>(InterlockedCompareExchange(&c->heartbeat, 0, 0))) < 5000;
}
}  // namespace pf::native
