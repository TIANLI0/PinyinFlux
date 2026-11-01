// SPDX-License-Identifier: MIT
#include "native_injector.h"
#include "../PinyinFlux.Inject/protocol.h"
#include "json.h"
#include "log.h"
#include "config.h"
#include "native_symbols.h"
#include <tlhelp32.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <memory>
#include <vector>

namespace pf {
namespace {
struct Entry {
  DWORD pid = 0, error = 0;
  HANDLE process = nullptr, mapping = nullptr;
  native::Channel* channel = nullptr;
  ULONG typingSeen[5]{};  // statistics counters already added to TypingStats
  ~Entry() {
    if (channel) UnmapViewOfFile(channel);
    if (mapping) CloseHandle(mapping);
    if (process) CloseHandle(process);
  }
};

bool SameUser(HANDLE process) {
  HANDLE caller = nullptr, target = nullptr;
  bool same = false;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &caller) && OpenProcessToken(process, TOKEN_QUERY, &target)) {
    DWORD a = 0, b = 0;
    GetTokenInformation(caller, TokenUser, nullptr, 0, &a);
    GetTokenInformation(target, TokenUser, nullptr, 0, &b);
    std::vector<BYTE> callerInfo(a), targetInfo(b);
    if (a && b && GetTokenInformation(caller, TokenUser, callerInfo.data(), a, &a) &&
        GetTokenInformation(target, TokenUser, targetInfo.data(), b, &b))
      same = EqualSid(reinterpret_cast<TOKEN_USER*>(callerInfo.data())->User.Sid,
                      reinterpret_cast<TOKEN_USER*>(targetInfo.data())->User.Sid) != FALSE;
  }
  if (caller) CloseHandle(caller);
  if (target) CloseHandle(target);
  return same;
}

uintptr_t Module(DWORD pid, const std::wstring& path) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  MODULEENTRY32W module{sizeof(module)};
  uintptr_t address = 0;
  for (BOOL ok = Module32FirstW(snapshot, &module); ok; ok = Module32NextW(snapshot, &module)) {
    if (_wcsicmp(module.szExePath, path.c_str()) == 0) { address = reinterpret_cast<uintptr_t>(module.modBaseAddr); break; }
  }
  CloseHandle(snapshot); return address;
}

std::wstring CacheDll() {
  wchar_t exe[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe))) return {};
  std::wstring source(exe);
  source = source.substr(0, source.find_last_of(L"\\/")) + L"\\PinyinFluxInject.dll";
  HANDLE file = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 8 * 1024 * 1024) { CloseHandle(file); return {}; }
  std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
  BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != bytes.size()) return {};
  BYTE hash[32]{}; DWORD hashSize = sizeof(hash);
  if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, bytes.data(), read, hash, &hashSize)) return {};
  std::wstring directory = DataDir() + L"\\inject";
  CreateDirectoryW(directory.c_str(), nullptr);
  directory += L"\\";
  constexpr wchar_t hex[] = L"0123456789abcdef";
  for (BYTE b : hash) { directory += hex[b >> 4]; directory += hex[b & 15]; }
  CreateDirectoryW(directory.c_str(), nullptr);
  std::wstring cached = directory + L"\\PinyinFluxInject.dll";
  // Loaded DLLs are immutable; upgrades use a different hash directory.
  if (!CopyFileW(source.c_str(), cached.c_str(), TRUE) && GetLastError() != ERROR_FILE_EXISTS) return {};
  HANDLE existing = CreateFileW(cached.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (existing == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER cachedSize{};
  bool same = GetFileSizeEx(existing, &cachedSize) && cachedSize.QuadPart == size.QuadPart;
  std::vector<BYTE> cachedBytes(bytes.size()); DWORD cachedRead = 0;
  same = same && ReadFile(existing, cachedBytes.data(), static_cast<DWORD>(cachedBytes.size()), &cachedRead, nullptr) &&
         cachedRead == read && cachedBytes == bytes;
  CloseHandle(existing);
  return same ? cached : std::wstring();
}

DWORD RemoteCall(HANDLE process, uintptr_t function, void* argument) {
  HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function), argument, 0, nullptr);
  if (!thread) return GetLastError();
  DWORD wait = WaitForSingleObject(thread, 5000);
  DWORD result = 0;
  if (wait != WAIT_OBJECT_0) result = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
  else if (!GetExitCodeThread(thread, &result)) result = GetLastError();
  CloseHandle(thread); return result;
}

DWORD Inject(Entry& entry, const std::wstring& dll, uintptr_t initializeRva) {
  constexpr DWORD rights = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE;
  entry.process = OpenProcess(rights, FALSE, entry.pid);
  if (!entry.process) return GetLastError();
  if (!SameUser(entry.process)) return ERROR_ACCESS_DENIED;
  wchar_t actual[MAX_PATH]{}, system[MAX_PATH]{}; DWORD length = ARRAYSIZE(actual);
  if (!QueryFullProcessImageNameW(entry.process, 0, actual, &length) || !GetSystemDirectoryW(system, ARRAYSIZE(system))) return GetLastError();
  if (_wcsicmp(actual, (std::wstring(system) + L"\\InputMethod\\CHS\\ChsIME.exe").c_str()) != 0 &&
      _wcsicmp(actual, (std::wstring(system) + L"\\ctfmon.exe").c_str()) != 0) return ERROR_BAD_PATHNAME;
  USHORT machine = 0, nativeMachine = 0;
  if (!IsWow64Process2(entry.process, &machine, &nativeMachine)) return GetLastError();
  if (machine != IMAGE_FILE_MACHINE_UNKNOWN || nativeMachine != IMAGE_FILE_MACHINE_AMD64) return ERROR_BAD_EXE_FORMAT;
  uintptr_t remote = Module(entry.pid, dll);
  if (!remote) {
    auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE containing = nullptr;
    if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(load), &containing)) return ERROR_PROC_NOT_FOUND;
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(containing, modulePath, ARRAYSIZE(modulePath));
    uintptr_t remoteModule = Module(entry.pid, modulePath);
    if (!remoteModule) return ERROR_MOD_NOT_FOUND;
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* text = VirtualAllocEx(entry.process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!text) return GetLastError();
    if (!WriteProcessMemory(entry.process, text, dll.c_str(), bytes, nullptr)) {
      DWORD error = GetLastError(); VirtualFreeEx(entry.process, text, 0, MEM_RELEASE); return error;
    }
    uintptr_t address = remoteModule + reinterpret_cast<uintptr_t>(load) - reinterpret_cast<uintptr_t>(containing);
    DWORD loaded = RemoteCall(entry.process, address, text);
    // Never free memory while a timed-out remote loader could still read it.
    if (loaded != ERROR_TIMEOUT) VirtualFreeEx(entry.process, text, 0, MEM_RELEASE);
    if (loaded == ERROR_TIMEOUT) return loaded;
    remote = Module(entry.pid, dll);
    if (!remote) return loaded ? ERROR_DLL_INIT_FAILED : ERROR_MOD_NOT_FOUND;
  }
  return RemoteCall(entry.process, remote + initializeRva, nullptr);
}
}  // namespace

NativeInjector::~NativeInjector() { Stop(); }
bool NativeInjector::Start(int port) {
  port_ = port;
  stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop_) return false;
  thread_ = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
  if (!thread_) { CloseHandle(stop_); stop_ = nullptr; return false; }
  return true;
}
void NativeInjector::Stop() {
  if (stop_) SetEvent(stop_);
  // The controller owns all resources it accesses until it has joined.
  if (thread_) { WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); thread_ = nullptr; }
  if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
}
DWORD WINAPI NativeInjector::Thread(void* self) { static_cast<NativeInjector*>(self)->Run(); return 0; }
void NativeInjector::Run() {
  std::wstring dll = CacheDll();
  if (dll.empty()) { lastError_ = ERROR_MOD_NOT_FOUND; errors_ = 1; return; }
  HMODULE image = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  auto initialize = image ? GetProcAddress(image, "PinyinFluxInitialize") : nullptr;
  uintptr_t rva = initialize ? reinterpret_cast<uintptr_t>(initialize) - reinterpret_cast<uintptr_t>(image) : 0;
  if (image) FreeLibrary(image);
  if (!rva) { lastError_ = ERROR_PROC_NOT_FOUND; errors_ = 1; return; }
  // Addresses for the installed Windows build, from Microsoft's public symbols.
  native::NativeLayout layout;
  std::string summary = ResolveNativeLayout(layout);
  { std::lock_guard lock(layoutMutex_); layoutSummary_ = summary; }
  std::vector<std::unique_ptr<Entry>> entries;
  DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
  DWORD scanned = 0, flushed = GetTickCount();
  while (WaitForSingleObject(stop_, 100) == WAIT_TIMEOUT) {
    if (GetTickCount() - scanned >= 2000) {
      scanned = GetTickCount();
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      PROCESSENTRY32W process{sizeof(process)};
      if (snapshot != INVALID_HANDLE_VALUE) {
        for (BOOL ok = Process32FirstW(snapshot, &process); ok; ok = Process32NextW(snapshot, &process)) {
          if (_wcsicmp(process.szExeFile, L"ChsIME.exe") != 0 && _wcsicmp(process.szExeFile, L"ctfmon.exe") != 0) continue;
          DWORD targetSession = 0;
          if (!ProcessIdToSessionId(process.th32ProcessID, &targetSession) || targetSession != session) continue;
          bool known = false;
          for (const auto& e : entries) if (e->pid == process.th32ProcessID) known = true;
          if (known) continue;
          wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, ARRAYSIZE(system));
          // Ignore dormant brokers which do not actually host the input engine.
          if (!Module(process.th32ProcessID, std::wstring(system) + L"\\InputService.dll") &&
              !Module(process.th32ProcessID, std::wstring(system) + L"\\InputMethod\\CHS\\ChsEM.dll")) {
            if (GetLastError() == ERROR_ACCESS_DENIED) {
              auto entry = std::make_unique<Entry>(); entry->pid = process.th32ProcessID; entry->error = ERROR_ACCESS_DENIED;
              entries.push_back(std::move(entry)); lastError_ = ERROR_ACCESS_DENIED;
            }
            continue;
          }
          auto entry = std::make_unique<Entry>(); entry->pid = process.th32ProcessID;
          entry->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(native::Channel), native::MappingName(entry->pid).c_str());
          bool created = entry->mapping && GetLastError() != ERROR_ALREADY_EXISTS;
          if (entry->mapping) entry->channel = static_cast<native::Channel*>(MapViewOfFile(entry->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(native::Channel)));
          if (entry->channel) {
            if (created) new (entry->channel) native::Channel();
            if (entry->channel->magic == native::kMagic) {
              entry->channel->layout = layout;
              entry->channel->port = static_cast<DWORD>(port_.load());
              InterlockedExchange(&entry->channel->heartbeat, static_cast<LONG>(GetTickCount()));
              InterlockedExchange(&entry->channel->enabled, 1);
              entry->error = Inject(*entry, dll, rva);
            } else entry->error = ERROR_INVALID_DATA;
          } else entry->error = GetLastError();
          if (entry->error) { lastError_ = entry->error; Log("native: pid %lu injection failed (%lu)", entry->pid, entry->error); }
          else Log("native: pid %lu initialized; waiting for validated candidate and network hooks", entry->pid);
          entries.push_back(std::move(entry));
        }
        CloseHandle(snapshot);
      }
    }
    LONG ready = 0, pending = 0, denied = 0, unsupported = 0, errors = 0, triggers = 0, requests = 0;
    LONG notifications = 0;
    for (const auto& entry : entries) {
      if (entry->process && WaitForSingleObject(entry->process, 0) == WAIT_OBJECT_0) continue;
      if (entry->error) { if (entry->error == ERROR_ACCESS_DENIED) ++denied; else ++errors; continue; }
      auto* c = entry->channel;
      c->port = static_cast<DWORD>(port_.load());
      InterlockedExchange(&c->heartbeat, static_cast<LONG>(GetTickCount()));
      LONG state = InterlockedCompareExchange(&c->state, 0, 0);
      if (LONG error = InterlockedCompareExchange(&c->error, 0, 0)) lastError_ = static_cast<DWORD>(error);
      if (state == native::Active) ++ready;
      else if (state == native::Unsupported) ++unsupported;
      else if (state == native::Failed || state == native::Stopped) ++errors;
      else ++pending;
      triggers += InterlockedCompareExchange(&c->triggers, 0, 0);
      requests += InterlockedCompareExchange(&c->requests, 0, 0);
      notifications += InterlockedCompareExchange(&c->notifications, 0, 0);
      lastFlags_ = InterlockedCompareExchange(&c->lastFlags, 0, 0);
      settingsResult_ = InterlockedCompareExchange(&c->settingsResult, 0, 0);
      for (int i = 0; i < 36; ++i) diagnostics_[i] = InterlockedCompareExchange(&c->diagnostics[i], 0, 0);
      InterlockedExchange(&c->statsEnabled, CurrentConfig()->typingStats ? 1 : 0);
      // The extension's counters only grow (and may wrap); take what's new since the last pass.
      volatile LONG* counters[5] = {&c->statCommits, &c->statChars, &c->statLetters, &c->statCloud, &c->statActiveMs};
      ULONG delta[5]{};
      for (int i = 0; i < 5; ++i) {
        ULONG now = static_cast<ULONG>(InterlockedCompareExchange(counters[i], 0, 0));
        delta[i] = now - entry->typingSeen[i];
        entry->typingSeen[i] = now;
      }
      typing_.Add({delta[0], delta[1], delta[2], delta[3], delta[4]});
    }
    if (GetTickCount() - flushed >= 60 * 1000) { flushed = GetTickCount(); typing_.Flush(); }
    ready_ = ready; pending_ = pending; denied_ = denied; unsupported_ = unsupported; errors_ = errors;
    triggers_ = triggers; requests_ = requests;
    notifications_ = notifications;
  }
  typing_.Flush();
  for (const auto& entry : entries) if (entry->channel) InterlockedExchange(&entry->channel->enabled, 0);
  // Wait for the DLL to restore its slots before a replacement version can
  // install new hooks in the same host. Callback storage remains mapped there.
  for (DWORD waited = 0; waited < 3000; waited += 50) {
    bool restoring = false;
    for (const auto& entry : entries) {
      if (!entry->channel || entry->error) continue;
      LONG state = InterlockedCompareExchange(&entry->channel->state, 0, 0);
      restoring |= state == native::Active || state == native::Waiting;
    }
    if (!restoring) break;
    Sleep(50);
  }
}
std::string NativeInjector::StatusJson() const {
  std::string summary;
  { std::lock_guard lock(layoutMutex_); summary = layoutSummary_; }
  std::string layoutJson;
  json::AppendQuoted(layoutJson, summary);
  return ",\"nativeMode\":true,\"nativeLayout\":" + layoutJson + ",\"nativeReady\":" + std::to_string(ready_.load()) +
      ",\"nativePending\":" + std::to_string(pending_.load()) + ",\"nativeDenied\":" + std::to_string(denied_.load()) +
      ",\"nativeUnsupported\":" + std::to_string(unsupported_.load()) + ",\"nativeErrors\":" + std::to_string(errors_.load()) +
      ",\"nativeLastError\":" + std::to_string(lastError_.load()) + ",\"nativeTriggers\":" + std::to_string(triggers_.load()) +
      ",\"nativeRequests\":" + std::to_string(requests_.load()) + ",\"nativeNotifications\":" + std::to_string(notifications_.load()) +
      ",\"nativeLastFlags\":" + std::to_string(lastFlags_.load()) + ",\"nativeSettingsResult\":" + std::to_string(settingsResult_.load()) +
      ",\"nativeQueryCalls\":" + std::to_string(diagnostics_[0].load()) + ",\"nativeQueryResult\":" + std::to_string(diagnostics_[1].load()) +
      ",\"nativeSuggestionCalls\":" + std::to_string(diagnostics_[2].load()) + ",\"nativeSuggestionResult\":" + std::to_string(diagnostics_[3].load()) +
      ",\"nativeBackendCalls\":" + std::to_string(diagnostics_[4].load()) + ",\"nativeBackendResult\":" + std::to_string(diagnostics_[5].load()) +
      ",\"nativeConnectCalls\":" + std::to_string(diagnostics_[6].load()) + ",\"nativeOpenCalls\":" + std::to_string(diagnostics_[7].load()) +
      ",\"nativeReadyCalls\":" + std::to_string(diagnostics_[8].load()) + ",\"nativeReadyResult\":" + std::to_string(diagnostics_[9].load()) +
      ",\"nativeReadyCount\":" + std::to_string(diagnostics_[10].load()) + ",\"nativeFilterCalls\":" + std::to_string(diagnostics_[11].load()) +
      ",\"nativeFilterBefore\":" + std::to_string(diagnostics_[12].load()) + ",\"nativeFilterAfter\":" + std::to_string(diagnostics_[13].load()) +
      ",\"nativeFilterResult\":" + std::to_string(diagnostics_[14].load()) + ",\"nativeAddCalls\":" + std::to_string(diagnostics_[15].load()) +
      ",\"nativeAddCount\":" + std::to_string(diagnostics_[16].load()) + ",\"nativeAddResult\":" + std::to_string(diagnostics_[17].load()) +
      ",\"nativeSpellResult\":" + std::to_string(diagnostics_[18].load()) + ",\"nativeSpellLength\":" + std::to_string(diagnostics_[19].load()) +
      ",\"nativeSpellMatches\":" + std::to_string(diagnostics_[20].load()) + ",\"nativeResponseCalls\":" + std::to_string(diagnostics_[21].load()) +
      ",\"nativeResponseFields\":" + std::to_string(diagnostics_[22].load()) + ",\"nativeResponseRecords\":" + std::to_string(diagnostics_[23].load()) +
      ",\"nativeResponseResult\":" + std::to_string(diagnostics_[24].load()) +
      ",\"nativeExcerptInput\":" + std::to_string(diagnostics_[25].load()) + ",\"nativeExcerptStored\":" + std::to_string(diagnostics_[26].load()) +
      ",\"nativeExcerptResult\":" + std::to_string(diagnostics_[27].load()) + ",\"nativeRefreshCalls\":" + std::to_string(diagnostics_[28].load()) +
      ",\"nativeRefreshResult\":" + std::to_string(diagnostics_[29].load()) + ",\"nativeOrderCalls\":" + std::to_string(diagnostics_[30].load()) +
      ",\"nativeOrder\":" + std::to_string(diagnostics_[31].load()) +
      ",\"nativeArrivals\":" + std::to_string(diagnostics_[32].load()) + ",\"nativeReplays\":" + std::to_string(diagnostics_[33].load()) +
      ",\"nativeArrivalWords\":" + std::to_string(diagnostics_[34].load()) + ",\"nativeDebounceSkips\":" + std::to_string(diagnostics_[35].load()) +
      ",\"typingEnabled\":" + (CurrentConfig()->typingStats ? "true" : "false") + typing_.Json();
}
}  // namespace pf
