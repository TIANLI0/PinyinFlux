// SPDX-License-Identifier: MIT
// Extends the native candidate pipeline. Microsoft Pinyin continues to own
// candidate rendering, ranking, selection and text composition.
#include "protocol.h"
#include <winhttp.h>
#include <objbase.h>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstring>
#include <MinHook.h>

namespace {
using namespace pf::native;
Channel* channel = nullptr;
std::atomic<bool> running{false}, live{false};
using Changed = HRESULT (WINAPI*)(void*, DWORD);
Changed originalChanged = nullptr;
Changed originalDesktopChanged = nullptr;
std::atomic<bool> cloudOptIn{false};
bool modern = false;
using Rule = HRESULT (WINAPI*)(void*);
Rule originalRule = nullptr, settingsRule = nullptr;
using Query = HRESULT (WINAPI*)(void*);
using Suggest = HRESULT (WINAPI*)(void*, const wchar_t*, DWORD, void*);
using Backend = HRESULT (WINAPI*)(void*, void*, void*);
Query desktopQuery = nullptr;
Suggest desktopSuggest = nullptr;
Backend backend = nullptr;
using ResultCallback = HRESULT (WINAPI*)(void*, void*);
using RecordCallback = HRESULT (WINAPI*)(void*, void*, void*);
ResultCallback readyCallback = nullptr, filterCallback = nullptr;
ResultCallback cachedCandidates = nullptr;
ResultCallback desktopRule = nullptr;
using Preload = HRESULT (WINAPI*)(void*, void**);
Preload preload = nullptr;
const GUID* listClass = nullptr;
const GUID* listInterface = nullptr;
ResultCallback setCloudExcerpts = nullptr;
Changed pinyinChanged = nullptr;
using DisplayOrder = DWORD (WINAPI*)(void*);
DisplayOrder cloudDisplayOrder = nullptr;
RecordCallback addCloud = nullptr, addResponse = nullptr, spellQuery = nullptr;
struct NativeVectorView { BYTE* begin; BYTE* end; BYTE* capacity; };
struct NativeStringView {
  union { wchar_t text[8]; const wchar_t* pointer; } data;
  size_t size, capacity;
  const wchar_t* Text() const { return capacity >= 8 ? data.pointer : data.text; }
};
std::vector<void*> detours;
std::mutex handlesMutex;
std::unordered_set<HINTERNET> connections, requests;
std::unordered_map<HINTERNET, HINTERNET> requestParents;
struct Patch { void** slot; void* before; void* after; };
std::vector<Patch> patches;
std::atomic<LONG> cloudWords{0};        // stored words of the latest response
std::atomic<bool> replayQuery{false};   // the next request repeats a settled spelling
thread_local void* desktopListener = nullptr;
using Debounce = int (WINAPI*)(void*, void*);
Debounce debounce = nullptr;
volatile BYTE* requestLock = nullptr;
NativeLayout layout;  // copied from the channel; resolved by the core for this machine
// Local candidate information interface, GUID_c4445657_6908_45eb_a6b9_2f1ea4b2a03a.
constexpr GUID kCandidateInfo = {0xc4445657, 0x6908, 0x45eb, {0xa6, 0xb9, 0x2f, 0x1e, 0xa4, 0xb2, 0xa0, 0x3a}};
// The native filter (0x1802c0cac) removes cloud words that are already local
// candidates, but returns early for a lone word, which then fills the
// placeholder as a blank. Compare against the local list before adding one.
std::mutex wordsMutex;
std::vector<std::wstring> localWords, responseWords;
ULONGLONG compositionStart = 0;  // first keystroke of the word being typed; engine thread only

bool Swap(void** slot, void* before, void* after) {
  DWORD protection = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
  void* previous = InterlockedCompareExchangePointer(slot, after, before);
  DWORD ignored;
  VirtualProtect(slot, sizeof(void*), protection, &ignored);
  return previous == before;
}
bool PatchSlot(void** slot, void* value) {
  void* before = *slot;
  patches.push_back({slot, before, value});
  if (!Swap(slot, before, value)) { patches.pop_back(); return false; }
  return true;
}
void Restore() {
  live = false;
  for (void* target : detours) MH_DisableHook(target);
  for (auto it = patches.rbegin(); it != patches.rend(); ++it) Swap(it->slot, it->after, it->before);
  patches.clear();
  // The DLL remains loaded: another thread may already have fetched a slot.
}
bool Detour(void* target, void* replacement, void** original) {
  if (std::find(detours.begin(), detours.end(), target) == detours.end()) {
    if (MH_CreateHook(target, replacement, original) != MH_OK) return false;
    detours.push_back(target);
  }
  return MH_EnableHook(target) == MH_OK;
}
HRESULT WINAPI DesktopQuery(void* self) {
  InterlockedIncrement(&channel->diagnostics[0]);
  HRESULT result = desktopQuery(self); InterlockedExchange(&channel->diagnostics[1], result); return result;
}
HRESULT WINAPI DesktopSuggest(void* self, const wchar_t* text, DWORD length, void* resultSink) {
  InterlockedIncrement(&channel->diagnostics[2]);
  HRESULT result = desktopSuggest(self, text, length, resultSink);
  InterlockedExchange(&channel->diagnostics[3], result); return result;
}
HRESULT WINAPI GetCandidates(void* self, void* request, void* result) {
  InterlockedIncrement(&channel->diagnostics[4]);
  HRESULT hr = backend(self, request, result); InterlockedExchange(&channel->diagnostics[5], hr); return hr;
}
LONG ListCount(void* list) {
  if (!list) return -1;
  using Count = HRESULT (WINAPI*)(void*, DWORD*);
  DWORD count = 0;
  return SUCCEEDED(reinterpret_cast<Count>((*static_cast<void***>(list))[3])(list, &count)) ? static_cast<LONG>(count) : -1;
}
LONG VectorCount(void* vector, size_t stride) {
  if (!vector) return -1;
  auto* view = static_cast<NativeVectorView*>(vector);
  auto begin = reinterpret_cast<uintptr_t>(view->begin), end = reinterpret_cast<uintptr_t>(view->end);
  return end >= begin && end - begin <= 1024 * stride ? static_cast<LONG>((end - begin) / stride) : -1;
}
HRESULT WINAPI SuggestionReady(void* self, void* list) {
  InterlockedIncrement(&channel->diagnostics[8]);
  InterlockedExchange(&channel->diagnostics[10], ListCount(list));
  HRESULT hr = readyCallback(self, list);
  InterlockedExchange(&channel->diagnostics[9], hr); return hr;
}
HRESULT WINAPI FilterSuggestions(void* self, void* list) {
  InterlockedIncrement(&channel->diagnostics[11]);
  InterlockedExchange(&channel->diagnostics[12], ListCount(list));
  HRESULT hr = filterCallback(self, list);
  InterlockedExchange(&channel->diagnostics[13], ListCount(list));
  InterlockedExchange(&channel->diagnostics[14], hr); return hr;
}
HRESULT WINAPI AddCloud(void* self, void* factory, void* records) {
  InterlockedIncrement(&channel->diagnostics[15]);
  InterlockedExchange(&channel->diagnostics[16], VectorCount(records, 0xf8));
  HRESULT hr = addCloud(self, factory, records);
  InterlockedExchange(&channel->diagnostics[17], hr); return hr;
}
// Copies native text that a different build may lay out differently; a bad
// guess must not take the input host down.
size_t CopyText(const wchar_t* text, size_t limit, wchar_t* out) {
  __try {
    size_t length = wcsnlen(text, limit);
    std::memcpy(out, text, length * sizeof(wchar_t));
    return length;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}
const wchar_t* RecordText(const BYTE* record, DWORD field, size_t& limit) {
  __try {
    WORD text = *reinterpret_cast<const WORD*>(record + field), next = *reinterpret_cast<const WORD*>(record + field + 2);
    if (text < 0x10 || next <= text || next - text > 130) return nullptr;
    limit = (next - text) / 2;
    return reinterpret_cast<const wchar_t*>(record + text);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}
HRESULT WINAPI AddResponse(void* self, void* fields, void* records) {
  InterlockedIncrement(&channel->diagnostics[21]);
  InterlockedExchange(&channel->diagnostics[22], VectorCount(fields, 0x40));
  HRESULT hr = addResponse(self, fields, records);
  LONG count = VectorCount(records, 0xf8);
  InterlockedExchange(&channel->diagnostics[23], count);
  // Each conversion record keeps its candidate text at +0x20 (see native_format_test).
  std::vector<std::wstring> words;
  for (LONG i = 0; SUCCEEDED(hr) && i < count && i < 16; ++i) {
    auto* text = reinterpret_cast<NativeStringView*>(static_cast<NativeVectorView*>(records)->begin + i * 0xf8 + 0x20);
    wchar_t buffer[64];
    if (text->size && text->size <= 64 && text->size <= text->capacity)
      if (size_t length = CopyText(text->Text(), text->size, buffer)) words.emplace_back(buffer, length);
  }
  { std::lock_guard lock(wordsMutex); responseWords = std::move(words); }
  InterlockedExchange(&channel->diagnostics[24], hr); return hr;
}
// Reads a local candidate as CheckCOCTriggerRule reads its first one: GetAt
// (method 4), QueryInterface for kCandidateInfo, then method 5 fills a
// 0x48-byte block (mask 0x80). Its +0x30 record, owned by the interface,
// holds byte offsets of the null-terminated text (field read from the rule's
// code, 0x0a) and of the next field.
bool LocalWord(void* list, DWORD index, std::wstring& word) {
  using GetAt = HRESULT (WINAPI*)(void*, DWORD, IUnknown**);
  using GetInfo = HRESULT (WINAPI*)(void*, BYTE*);
  IUnknown* item = nullptr;
  if (FAILED(reinterpret_cast<GetAt>((*static_cast<void***>(list))[4])(list, index, &item)) || !item) return false;
  void* info = nullptr;
  if (SUCCEEDED(item->QueryInterface(kCandidateInfo, &info)) && info) {
    alignas(8) BYTE block[0x48]{};
    *reinterpret_cast<DWORD*>(block) = 0x80;
    if (SUCCEEDED(reinterpret_cast<GetInfo>((*static_cast<void***>(info))[5])(info, block))) {
      const BYTE* record = *reinterpret_cast<BYTE**>(block + 0x30);
      size_t limit = 0;
      const wchar_t* text = record ? RecordText(record, layout.engine.candidateText, limit) : nullptr;
      wchar_t buffer[65];
      if (text) word.assign(buffer, CopyText(text, limit, buffer));
    }
    static_cast<IUnknown*>(info)->Release();
  }
  item->Release();
  return !word.empty();
}
void RememberLocalWords(void* list) {
  std::vector<std::wstring> words;
  using Count = HRESULT (WINAPI*)(void*, DWORD*);
  DWORD count = 0;
  if (list && layout.engine.candidateText && SUCCEEDED(reinterpret_cast<Count>((*static_cast<void***>(list))[3])(list, &count)))
    for (DWORD i = 0; i < count && i < 256; ++i) {
      std::wstring word;
      if (LocalWord(list, i, word)) words.push_back(std::move(word));
    }
  std::lock_guard lock(wordsMutex);
  localWords = std::move(words);
}
bool AddsCloudWord() {
  std::lock_guard lock(wordsMutex);
  return std::any_of(responseWords.begin(), responseWords.end(), [](const std::wstring& word) {
    return std::find(localWords.begin(), localWords.end(), word) == localWords.end();
  });
}
HRESULT WINAPI CachedCandidates(void* self, void* output) {
  // The private cache also stores empty/error responses and outlives our core.
  // Use the provider-aware cache in the core while our replacement is active,
  // so a result from a previous DLL/provider configuration cannot suppress it.
  if (live.load() && Enabled(channel)) {
    if (output) *static_cast<void**>(output) = nullptr;
    return E_FAIL;  // The verified caller interprets failure as a cache miss.
  }
  return cachedCandidates(self, output);
}
HRESULT WINAPI SetCloudExcerpts(void* self, void* excerpts) {
  InterlockedExchange(&channel->diagnostics[25], VectorCount(excerpts, sizeof(void*)));
  HRESULT hr = setCloudExcerpts(self, excerpts);
  LONG stored = VectorCount(static_cast<BYTE*>(self) + layout.engine.excerpts, 0xc8);
  InterlockedExchange(&channel->diagnostics[26], stored);
  cloudWords = stored;
  InterlockedExchange(&channel->diagnostics[27], hr); return hr;
}
// The engine adds a blank cloud placeholder whenever it regenerates the
// candidates of a spelling (0x8) and fills it when a response arrives
// (0x1000); an arrival without a placeholder is dropped. Add the placeholder
// only once a displayable word exists: replay the keystroke for the same
// spelling to both listeners, on this thread, so the blank is never shown
// while waiting or when the cloud has nothing to add. The replayed query is
// answered from the core's cache without the native typing debounce.
thread_local bool regenerating = false;
bool replayed = false;  // engine notification thread only
HRESULT WINAPI PinyinChanged(void* self, DWORD flags) {
  if ((flags & 0x8) && !regenerating && !compositionStart) compositionStart = GetTickCount64();  // a keystroke
  if (flags & 0x1000) {
    InterlockedIncrement(&channel->diagnostics[32]);
    InterlockedExchange(&channel->diagnostics[34], cloudWords.load());
  }
  if (flags & 0x400) InterlockedIncrement(&channel->diagnostics[28]);
  if ((flags & 0x1000) && !replayed && cloudWords.load() > 0 && desktopListener && live.load() && Enabled(channel) &&
      AddsCloudWord()) {
    replayed = true;
    regenerating = true;
    replayQuery = true;
    HRESULT replay = pinyinChanged(self, 0x8);
    if (SUCCEEDED(replay)) replay = originalDesktopChanged(desktopListener, 0x8);
    if (SUCCEEDED(replay)) replay = pinyinChanged(self, 0x22000);
    if (SUCCEEDED(replay)) replay = originalDesktopChanged(desktopListener, 0x22000);
    regenerating = false;
    InterlockedIncrement(&channel->diagnostics[33]);
    // This arrival answers the query that had no placeholder.
    if (SUCCEEDED(replay)) return S_OK;
    replayQuery = false;
  }
  HRESULT hr = pinyinChanged(self, flags);
  if (flags & 0x400) InterlockedExchange(&channel->diagnostics[29], hr);
  return hr;
}
HRESULT WINAPI DesktopRule(void* self, void* localSuggestions) {
  HRESULT nativeResult = desktopRule(self, localSuggestions);
  if (!regenerating) { replayed = false; replayQuery = false; cloudWords = 0; }  // a keystroke
  if (FAILED(nativeResult)) return nativeResult;
  if (!live.load() || !Enabled(channel)) return nativeResult;
  if (!cloudOptIn.load()) return nativeResult;
  auto* object = static_cast<BYTE*>(self);
  const auto& e = layout.engine;
  // Keep the consent/profile/input-scope gates of the native rule (offsets
  // read from its code by the core). Only the quota, network-cost and
  // local-candidate-confidence heuristics are replaced: our core provides its
  // own timeout and provider-aware cache.
  if (!object[e.ruleFlags[0]] || (object[e.ruleFlags[1]] && !object[e.ruleFlags[2]])) return nativeResult;
  void* settings = *reinterpret_cast<void**>(object + e.ruleSettings);
  if (!settings) return nativeResult;
  auto** table = *static_cast<void***>(settings);
  using GetScope = HRESULT (WINAPI*)(void*, DWORD*);
  DWORD scope = 0;
  if (FAILED(reinterpret_cast<GetScope>(table[e.settingsSlots[0] / 8])(settings, &scope)) || scope == 2 ||
      reinterpret_cast<Rule>(table[e.settingsSlots[1] / 8])(settings) == 0 ||
      reinterpret_cast<Rule>(table[e.settingsSlots[2] / 8])(settings) != S_FALSE ||
      reinterpret_cast<Rule>(table[e.settingsSlots[3] / 8])(settings) != S_FALSE) return nativeResult;
  void* pool = *reinterpret_cast<void**>(object + e.rulePool);
  using GetStringRef = HRESULT (WINAPI*)(void*, DWORD, const wchar_t**, DWORD*);
  const wchar_t* text = nullptr; DWORD length = 0;
  if (!pool || FAILED(reinterpret_cast<GetStringRef>((*static_cast<void***>(pool))[e.poolSpelling / 8])(pool, 8, &text, &length)) ||
      !text || length > 128) return nativeResult;
  DWORD letters = 0;
  for (DWORD i = 0; i < length; ++i)
    if ((text[i] >= L'a' && text[i] <= L'z') || (text[i] >= L'A' && text[i] <= L'Z')) ++letters;
    else if (text[i] != L'\'' && text[i] != L' ') return nativeResult;
  if (letters < 4) return S_FALSE;
  if (!regenerating) RememberLocalWords(localSuggestions);
  if (nativeResult != S_FALSE) return nativeResult;
  InterlockedIncrement(&channel->triggers);
  return S_OK;
}
HRESULT WINAPI PreloadingList(void* self, void** output) {
  if (!live.load() || !Enabled(channel) || regenerating) return preload(self, output);
  if (!output) return E_INVALIDARG;
  *output = nullptr;
  // The original caller concatenates this list with its local suggestions.
  // An empty, real native list preserves local numbering while a cloud query
  // is pending; see PinyinChanged for how a displayable response is shown.
  return CoCreateInstance(*listClass, nullptr, CLSCTX_INPROC_SERVER, *listInterface, output);
}
DWORD WINAPI CloudDisplayOrder(void* self) {
  InterlockedIncrement(&channel->diagnostics[30]);
  DWORD order = cloudDisplayOrder(self);
  InterlockedExchange(&channel->diagnostics[31], static_cast<LONG>(order)); return order;
}
HRESULT WINAPI SpellQuery(void* self, void* list, void* output) {
  HRESULT hr = spellQuery(self, list, output);
  InterlockedExchange(&channel->diagnostics[18], hr);
  auto* spelling = static_cast<NativeStringView*>(output);
  InterlockedExchange(&channel->diagnostics[19], SUCCEEDED(hr) ? static_cast<LONG>(spelling->size) : -1);
  return hr;
}

HRESULT WINAPI OnChanged(void* self, DWORD flags) {
  InterlockedIncrement(&channel->notifications);
  InterlockedExchange(&channel->lastFlags, static_cast<LONG>(flags));
  // Validated against ChsEM 10.0.26100.9767 public PDB + disassembly:
  // bit 3 = spelling changed, bit 29 = request native cloud candidates.
  // Preserve the native generation checks, debounce, cancellation and UI.
  if (live.load() && Enabled(channel) && (flags & (modern ? 0x10 : 8)) &&
      (!modern || settingsRule(static_cast<BYTE*>(self) - layout.engine.cloudBase) == S_OK)) {
    flags |= modern ? 0x20000 : 0x20000000;
    InterlockedIncrement(&channel->triggers);
  }
  return originalChanged(self, flags);
}
HRESULT WINAPI OnDesktopChanged(void* self, DWORD flags) {
  desktopListener = self;
  InterlockedIncrement(&channel->notifications);
  InterlockedExchange(&channel->lastFlags, static_cast<LONG>(flags));
  // The upstream rule owns the acceptance flag as well as the query bit.
  // Promoting only this downstream notification leaves cloud results invisible.
  return originalDesktopChanged(self, flags);
}
HRESULT WINAPI CandidateRule(void* self) {
  // The original setting rule includes cloud opt-in and sensitive input scopes.
  // Only bypass candidate-confidence/daily-quota heuristics for active queries.
  if (live.load() && Enabled(channel)) return settingsRule(self);
  return originalRule(self);
}
bool Bing(const wchar_t* host) {
  return host && (_wcsicmp(host, L"cn.bing.com") == 0 || _wcsicmp(host, L"www.bing.com") == 0 ||
                  _wcsicmp(host, L"cloudsuggestion.chinacloudsites.cn") == 0);
}
HINTERNET WINAPI Connect(HINTERNET session, LPCWSTR server, INTERNET_PORT port, DWORD reserved) {
  InterlockedIncrement(&channel->diagnostics[6]);
  bool redirect = live.load() && Enabled(channel) && Bing(server);
  // Preserve the IME's own connection handle and original server. Restoring
  // hooks must not strand a cached connection on our stopped loopback service.
  HINTERNET result = WinHttpConnect(session, server, port, reserved);
  if (result && redirect) { std::lock_guard lock(handlesMutex); connections.insert(result); }
  return result;
}
HINTERNET WINAPI Open(HINTERNET connection, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
                       LPCWSTR referrer, LPCWSTR* accept, DWORD flags) {
  InterlockedIncrement(&channel->diagnostics[7]);
  bool known;
  { std::lock_guard lock(handlesMutex); known = connections.contains(connection); }
  bool redirect = false;
  HINTERNET result = WinHttpOpenRequest(connection, verb, object, version, referrer, accept, flags);
  HINTERNET replacementConnection = nullptr;
  if (result && live.load() && Enabled(channel)) {
    // The IME may reuse a connection which predates DLL injection. Inspect the
    // newly opened request before any I/O, then recreate it on loopback.
    wchar_t url[4096]{}; DWORD bytes = sizeof(url);
    bool wanted = known;
    if (!wanted && WinHttpQueryOption(result, WINHTTP_OPTION_URL, url, &bytes)) {
      wchar_t host[256]{}; URL_COMPONENTS parts{sizeof(parts)};
      parts.lpszHostName = host; parts.dwHostNameLength = ARRAYSIZE(host);
      wanted = WinHttpCrackUrl(url, 0, 0, &parts) && Bing(host);
    }
    if (wanted) {
        HINTERNET session = nullptr; DWORD size = sizeof(session);
        if (WinHttpQueryOption(connection, WINHTTP_OPTION_PARENT_HANDLE, &session, &size)) {
          replacementConnection = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(channel->port), 0);
          if (replacementConnection) {
            HINTERNET replacement = WinHttpOpenRequest(replacementConnection, verb, object, version, referrer, accept, flags & ~WINHTTP_FLAG_SECURE);
            if (replacement) { WinHttpCloseHandle(result); result = replacement; redirect = true; }
            else { WinHttpCloseHandle(replacementConnection); replacementConnection = nullptr; }
          }
        }
    }
  }
  if (result && redirect) {
    { std::lock_guard lock(handlesMutex); requests.insert(result);
      if (replacementConnection) requestParents.emplace(result, replacementConnection);
    }
    WINHTTP_PROXY_INFO direct{WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr};
    WinHttpSetOption(result, WINHTTP_OPTION_PROXY, &direct, sizeof(direct));
    InterlockedIncrement(&channel->requests);
  }
  return result;
}
BOOL WINAPI Option(HINTERNET handle, DWORD option, LPVOID buffer, DWORD size) {
  if (option == WINHTTP_OPTION_PROXY) {
    bool redirected;
    { std::lock_guard lock(handlesMutex); redirected = requests.contains(handle) || connections.contains(handle); }
    if (redirected) {
      WINHTTP_PROXY_INFO direct{WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr};
      return WinHttpSetOption(handle, option, &direct, sizeof(direct));
    }
  }
  return WinHttpSetOption(handle, option, buffer, size);
}
BOOL WINAPI Close(HINTERNET handle) {
  HINTERNET parent = nullptr;
  { std::lock_guard lock(handlesMutex); requests.erase(handle); connections.erase(handle);
    auto it = requestParents.find(handle);
    if (it != requestParents.end()) { parent = it->second; requestParents.erase(it); }
  }
  BOOL result = WinHttpCloseHandle(handle);
  if (parent) WinHttpCloseHandle(parent);
  return result;
}

// Typing statistics. SetPrevPrimitive receives each committed conversion and
// its reading, as context for next-word prediction; it declines (S_FALSE)
// where Microsoft Pinyin keeps no history, and so do we. Only counts leave
// this function. Engine thread only.
using CommitText = HRESULT (WINAPI*)(void*, const NativeStringView*, const NativeStringView*);
CommitText commitText = nullptr;
ULONGLONG lastCommit = 0;
constexpr ULONGLONG kTypingPause = 30000;  // a longer gap between words is a break, not typing
struct Committed { wchar_t text[64]; size_t length; LONG chars, letters; };
bool Han(wchar_t c) { return (c >= 0x3400 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xd840 && c <= 0xd87f); }
bool ReadCommitted(const NativeStringView* text, const NativeStringView* reading, Committed& out) {
  __try {
    out.length = text->size; out.chars = 0; out.letters = 0;
    if (out.length == 0 || out.length > 256) return false;
    const wchar_t* t = text->Text();
    for (size_t i = 0; i < out.length; ++i) {
      if (i < ARRAYSIZE(out.text)) out.text[i] = t[i];
      if (Han(t[i])) ++out.chars;  // a supplementary character counts once, by its high surrogate
    }
    if (reading && reading->size <= 256) {
      const wchar_t* r = reading->Text();
      for (size_t i = 0; i < reading->size; ++i) out.letters += (r[i] >= L'a' && r[i] <= L'z') || (r[i] >= L'A' && r[i] <= L'Z');
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void CountCommit(const Committed& c) {
  ULONGLONG now = GetTickCount64();
  ULONGLONG gap = lastCommit ? now - lastCommit : kTypingPause + 1;
  // After a break, count from the first keystroke of this word instead.
  if (gap > kTypingPause) gap = compositionStart && now - compositionStart <= kTypingPause ? now - compositionStart : 0;
  lastCommit = now; compositionStart = 0;
  // A cloud word is one the placeholder actually showed for this spelling: from
  // our response and not among the local candidates (those the native filter drops).
  bool cloud = false;
  {
    std::wstring_view word(c.text, c.length <= ARRAYSIZE(c.text) ? c.length : 0);
    std::lock_guard lock(wordsMutex);
    cloud = replayed && !word.empty() &&
        std::find(responseWords.begin(), responseWords.end(), word) != responseWords.end() &&
        std::find(localWords.begin(), localWords.end(), word) == localWords.end();
    responseWords.clear();  // answered this spelling; never match a later word
  }
  InterlockedIncrement(&channel->statCommits);
  InterlockedExchangeAdd(&channel->statChars, c.chars);
  InterlockedExchangeAdd(&channel->statLetters, c.letters);
  InterlockedExchangeAdd(&channel->statActiveMs, static_cast<LONG>(gap));
  if (cloud) InterlockedIncrement(&channel->statCloud);
}
HRESULT WINAPI CommitTextHook(void* self, const NativeStringView* text, const NativeStringView* reading) {
  HRESULT hr = commitText(self, text, reading);
  Committed c;
  if (hr == S_OK && live.load() && Enabled(channel) && InterlockedCompareExchange(&channel->statsEnabled, 0, 0) &&
      ReadCommitted(text, reading, c) && c.chars > 0)
    CountCommit(c);
  return hr;
}

// ChsProxyDS waits until typing has paused (polling every 10 ms) before it
// sends a request. A replayed query repeats a spelling the user already
// paused on: take the same single-request lock and send right away.
int WINAPI SkipDebounce(void* self, void* input) {
  if (!replayQuery.exchange(false) || !live.load() || !Enabled(channel)) return debounce(self, input);
  while (*requestLock) Sleep(10);
  *requestLock = 1;
  InterlockedIncrement(&channel->diagnostics[35]);
  return 1;
}
IMAGE_NT_HEADERS* Headers(HMODULE module) {
  auto* bytes = reinterpret_cast<BYTE*>(module);
  return reinterpret_cast<IMAGE_NT_HEADERS*>(bytes + reinterpret_cast<IMAGE_DOS_HEADER*>(bytes)->e_lfanew);
}
bool SystemModule(HMODULE module, const wchar_t* relative) {
  wchar_t actual[MAX_PATH]{}, directory[MAX_PATH]{};
  if (!GetModuleFileNameW(module, actual, ARRAYSIZE(actual)) || !GetSystemDirectoryW(directory, ARRAYSIZE(directory))) return false;
  std::wstring expected = std::wstring(directory) + L"\\" + relative;
  return _wcsicmp(actual, expected.c_str()) == 0;
}
// The layout describes exactly one build of each image: it must be the one loaded.
bool SameImage(HMODULE module, const ImageId& image) {
  auto* head = Headers(module);
  return head->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
         ImageId{head->FileHeader.TimeDateStamp, head->OptionalHeader.SizeOfImage} == image;
}
// The loaded code must still be what the core read from disk; otherwise
// another tool patched it and we leave it alone.
BYTE* Locate(BYTE* base, const Function& function) {
  return function.rva && std::memcmp(base + function.rva, function.entry, kEntryBytes) == 0 ? base + function.rva : nullptr;
}
bool Hook(BYTE* base, const Function& function, void* replacement, void** original) {
  BYTE* target = Locate(base, function);
  return target && Detour(target, replacement, original);
}
bool InstallTrigger(HMODULE module, bool isModern) {
  if (!SystemModule(module, isModern ? L"InputService.dll" : L"InputMethod\\CHS\\ChsEM.dll")) return false;
  auto* base = reinterpret_cast<BYTE*>(module);
  modern = isModern;
  MH_STATUS initialized = MH_Initialize();
  if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
  originalChanged = nullptr;
  if (!modern) {
    if (!SameImage(module, layout.legacy.image)) return false;
    originalChanged = reinterpret_cast<Changed>(Locate(base, layout.legacy.cloudChanged));
    if (!originalChanged) return false;
  } else {
    const auto& e = layout.engine;
    if (!SameImage(module, e.image) || !e.pinyinChanged.rva) return false;
    listClass = reinterpret_cast<const GUID*>(base + e.listClass);
    listInterface = reinterpret_cast<const GUID*>(base + e.listInterface);
    originalDesktopChanged = reinterpret_cast<Changed>(Locate(base, e.desktopChanged));
    if (!originalDesktopChanged ||
        !Hook(base, e.setExcerpts, reinterpret_cast<void*>(SetCloudExcerpts), reinterpret_cast<void**>(&setCloudExcerpts)) ||
        !Hook(base, e.pinyinChanged, reinterpret_cast<void*>(PinyinChanged), reinterpret_cast<void**>(&pinyinChanged)) ||
        !Hook(base, e.triggerRule, reinterpret_cast<void*>(DesktopRule), reinterpret_cast<void**>(&desktopRule)) ||
        !Hook(base, e.preloading, reinterpret_cast<void*>(PreloadingList), reinterpret_cast<void**>(&preload))) return false;
    // The ChsIME handler path (not used by the desktop candidate window).
    settingsRule = reinterpret_cast<Rule>(Locate(base, e.settingRule));
    if (settingsRule && e.cloudBase &&
        Hook(base, e.candidateRule, reinterpret_cast<void*>(CandidateRule), reinterpret_cast<void**>(&originalRule)))
      originalChanged = reinterpret_cast<Changed>(Locate(base, e.cloudChanged));
    // Diagnostics only.
    Hook(base, e.doCloudQuery, reinterpret_cast<void*>(DesktopQuery), reinterpret_cast<void**>(&desktopQuery));
    Hook(base, e.requestSuggestion, reinterpret_cast<void*>(DesktopSuggest), reinterpret_cast<void**>(&desktopSuggest));
    Hook(base, e.suggestionReady, reinterpret_cast<void*>(SuggestionReady), reinterpret_cast<void**>(&readyCallback));
    Hook(base, e.filterSuggestions, reinterpret_cast<void*>(FilterSuggestions), reinterpret_cast<void**>(&filterCallback));
    Hook(base, e.spellQuery, reinterpret_cast<void*>(SpellQuery), reinterpret_cast<void**>(&spellQuery));
    Hook(base, e.displayOrder, reinterpret_cast<void*>(CloudDisplayOrder), reinterpret_cast<void**>(&cloudDisplayOrder));
    // Typing statistics; the core enables counting.
    Hook(base, e.commitText, reinterpret_cast<void*>(CommitTextHook), reinterpret_cast<void**>(&commitText));
  }
  auto* head = Headers(module);
  auto* sections = IMAGE_FIRST_SECTION(head);
  LONG count = 0;
  for (WORD i = 0; i < head->FileHeader.NumberOfSections; ++i) {
    if (std::memcmp(sections[i].Name, ".rdata", 6) != 0) continue;
    auto** begin = reinterpret_cast<void**>(base + sections[i].VirtualAddress);
    size_t size = sections[i].Misc.VirtualSize / sizeof(void*);
    for (size_t j = 0; j < size; ++j) {
      if (originalChanged && begin[j] == reinterpret_cast<void*>(originalChanged) &&
          PatchSlot(begin + j, reinterpret_cast<void*>(OnChanged))) ++count;
      else if (modern && begin[j] == reinterpret_cast<void*>(originalDesktopChanged) &&
               PatchSlot(begin + j, reinterpret_cast<void*>(OnDesktopChanged))) ++count;
    }
  }
  InterlockedExchange(&channel->triggerSlots, count);
  return count > 0;
}
bool InstallNetwork(HMODULE module) {
  if (!SystemModule(module, L"InputMethod\\CHS\\ChsProxyDS.dll")) return false;
  auto* base = reinterpret_cast<BYTE*>(module);
  // Request redirection below works on any build; these hooks refine it when
  // the layout matches.
  const auto& p = layout.proxy;
  if (SameImage(module, p.image)) {
    MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    Hook(base, p.getCandidates, reinterpret_cast<void*>(GetCandidates), reinterpret_cast<void**>(&backend));
    Hook(base, p.localCache, reinterpret_cast<void*>(CachedCandidates), reinterpret_cast<void**>(&cachedCandidates));
    Hook(base, p.addCloud, reinterpret_cast<void*>(AddCloud), reinterpret_cast<void**>(&addCloud));
    Hook(base, p.addResponse, reinterpret_cast<void*>(AddResponse), reinterpret_cast<void**>(&addResponse));
    if (p.requestLock) {
      requestLock = base + p.requestLock;
      Hook(base, p.debounce, reinterpret_cast<void*>(SkipDebounce), reinterpret_cast<void**>(&debounce));
    }
  }
  auto dir = Headers(module)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
  LONG count = 0;
  for (; imports->Name; ++imports) {
    if (_stricmp(reinterpret_cast<char*>(base + imports->Name), "winhttp.dll") != 0 || !imports->OriginalFirstThunk) continue;
    auto* name = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk);
    auto* address = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
    for (; name->u1.AddressOfData; ++name, ++address) {
      if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)) continue;
      auto* entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData);
      void* replacement = nullptr;
      if (std::strcmp(entry->Name, "WinHttpConnect") == 0) replacement = reinterpret_cast<void*>(Connect);
      else if (std::strcmp(entry->Name, "WinHttpOpenRequest") == 0) replacement = reinterpret_cast<void*>(Open);
      else if (std::strcmp(entry->Name, "WinHttpSetOption") == 0) replacement = reinterpret_cast<void*>(Option);
      else if (std::strcmp(entry->Name, "WinHttpCloseHandle") == 0) replacement = reinterpret_cast<void*>(Close);
      if (replacement && PatchSlot(reinterpret_cast<void**>(&address->u1.Function), replacement)) ++count;
    }
  }
  InterlockedExchange(&channel->networkSlots, count);
  return count == 4;
}

DWORD RunWorker() {
  // Mapping + DLL remain valid until process exit, including during callbacks
  // which had already fetched a vtable/IAT slot when Stop restored that slot.
  // Always attach to the current core's mapping: once the previous core closed
  // its handle, the name refers to a new section although our old view keeps
  // the old one alive. Old views are intentionally never unmapped.
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MappingName(GetCurrentProcessId()).c_str());
  if (mapping) {
    if (auto* current = static_cast<Channel*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Channel))))
      channel = current;
    CloseHandle(mapping);
  }
  if (!channel || channel->magic != kMagic) { running = false; return ERROR_INVALID_DATA; }
  layout = channel->layout;
  bool trigger = false, network = false;
  while (Enabled(channel)) {
    DWORD cloud = 0, bytes = sizeof(cloud);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\InputMethod\\Settings\\CHS", L"Enable Cloud Candidate",
        RRF_RT_REG_DWORD, nullptr, &cloud, &bytes);
    DWORD policy = 0; bytes = sizeof(policy);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputMethod\\Settings\\Shared", L"Enable Cloud Candidate",
        RRF_RT_REG_DWORD, nullptr, &policy, &bytes) == ERROR_SUCCESS) cloud = policy;
    cloudOptIn = cloud != 0;
    HMODULE engine = GetModuleHandleW(L"InputService.dll");
    bool isModern = engine != nullptr;
    if (!engine) engine = GetModuleHandleW(L"ChsEM.dll");
    HMODULE proxy = GetModuleHandleW(L"ChsProxyDS.dll");
    if (engine && !trigger) {
      if (!InstallTrigger(engine, isModern)) {
        InterlockedExchange(&channel->error, ERROR_REVISION_MISMATCH);
        InterlockedExchange(&channel->state, Unsupported); Restore(); running = false; return ERROR_REVISION_MISMATCH;
      }
      trigger = true;
    }
    if (proxy && !network) {
      if (!InstallNetwork(proxy)) {
        InterlockedExchange(&channel->error, ERROR_INVALID_FUNCTION);
        InterlockedExchange(&channel->state, Failed); Restore(); running = false; return ERROR_INVALID_FUNCTION;
      }
      network = true;
    }
    // Native cloud needs to be able to trigger before ChsProxyDS is first loaded.
    live = trigger;
    InterlockedExchange(&channel->state, trigger && network ? Active : Waiting);
    Sleep(100);
  }
  // Hooks are restored before Stopped is published; a restarted core may
  // initialize again as soon as it observes Stopped.
  Restore(); running = false;
  InterlockedExchange(&channel->state, Stopped);
  return ERROR_SUCCESS;
}
DWORD WINAPI Worker(void*) {
  try { return RunWorker(); }
  catch (...) {
    Restore();
    if (channel) {
      InterlockedExchange(&channel->error, ERROR_NOT_ENOUGH_MEMORY);
      InterlockedExchange(&channel->state, Failed);
    }
    running = false;
    return ERROR_NOT_ENOUGH_MEMORY;
  }
}
}  // namespace

// Invoked AFTER LoadLibrary has finished. No hooks or work in DllMain.
extern "C" __declspec(dllexport) DWORD WINAPI PinyinFluxInitialize(void*) {
  if (running.exchange(true)) return ERROR_ALREADY_INITIALIZED;
  HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  if (!thread) { running = false; return GetLastError(); }
  CloseHandle(thread); return ERROR_SUCCESS;
}
