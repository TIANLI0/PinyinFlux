// SPDX-License-Identifier: MIT
#include "native_compat.h"

#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "common.h"
#include "log.h"
#include "native_symbols.h"

namespace pf {

namespace {

constexpr DWORD kPreflightMs = 20000;
constexpr size_t kRemembered = 16;  // combinations kept in compat.ini
const char* const kVerdicts[] = {"unknown", "pass", "fail", "crashed"};

void Say(const std::string& line) {
  std::string text = line + "\n";
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

// The extension patches ChsProxyDS's WinHTTP imports; find one of them.
void** Import(HMODULE module, const char* wanted) {
  auto* base = reinterpret_cast<BYTE*>(module);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  DWORD directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (!directory) return nullptr;
  for (auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory); imports->Name; ++imports) {
    if (_stricmp(reinterpret_cast<char*>(base + imports->Name), "winhttp.dll") != 0 || !imports->OriginalFirstThunk) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(name->Name, wanted) == 0) return reinterpret_cast<void**>(&slots->u1.Function);
    }
  }
  return nullptr;
}

void Beat(native::Channel* channel) { InterlockedExchange(&channel->heartbeat, static_cast<LONG>(GetTickCount())); }

// Waits while the extension is in `state`, keeping the heartbeat fresh.
LONG WaitWhile(native::Channel* channel, LONG state, DWORD ms) {
  for (DWORD waited = 0; waited < ms && InterlockedCompareExchange(&channel->state, 0, 0) == state; waited += 20) {
    Beat(channel);
    Sleep(20);
  }
  return InterlockedCompareExchange(&channel->state, 0, 0);
}

std::string Hex(DWORD value) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08lx", value);
  return text;
}

}  // namespace

int RunPreflight(const std::wstring& extension) {
  auto fail = [](const std::string& step, const std::string& detail) {
    Say("FAIL " + step + ": " + detail);
    return 1;
  };
  // Nothing to check where the Chinese IME is not installed (Windows Server CI
  // images): 77 is CTest's skip code; the core sees any other answer than PASS as a failure.
  wchar_t buffer[MAX_PATH]{};
  GetSystemDirectoryW(buffer, MAX_PATH);
  const std::wstring system = buffer;
  if (!ImageOnDisk(system + L"\\InputMethod\\CHS\\ChsProxyDS.dll").stamp ||
      (!ImageOnDisk(system + L"\\InputService.dll").stamp && !ImageOnDisk(system + L"\\InputMethod\\CHS\\ChsEM.dll").stamp)) {
    Say("SKIP Microsoft Pinyin is not installed");
    return 77;
  }
  // A loopback stand-in for the core, answering one cloud request.
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int length = sizeof(address);
  if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
      listen(listener, 1) || getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length))
    return fail("setup", "no loopback listener");

  // The channel the extension looks for in its host, here this process.
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(native::Channel),
                                      native::MappingName(GetCurrentProcessId()).c_str());
  auto* channel = mapping ? static_cast<native::Channel*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(native::Channel))) : nullptr;
  if (!channel) return fail("setup", "no channel (" + std::to_string(GetLastError()) + ")");
  new (channel) native::Channel();
  channel->port = ntohs(address.sin_port);
  LayoutReport report;
  std::string summary = ResolveNativeLayout(channel->layout, &report);
  if (!report.usable) return fail("layout", summary);
  Beat(channel);

  // The images as installed, mapped but never run (no DllMain, no imports).
  const bool modern = channel->layout.engine.pinyinChanged.rva != 0;
  HMODULE engine = LoadLibraryExW((modern ? system + L"\\InputService.dll" : system + L"\\InputMethod\\CHS\\ChsEM.dll").c_str(),
                                  nullptr, DONT_RESOLVE_DLL_REFERENCES);
  HMODULE proxy = LoadLibraryExW((system + L"\\InputMethod\\CHS\\ChsProxyDS.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!engine || !proxy) return fail("load", "Microsoft Pinyin images (" + std::to_string(GetLastError()) + ")");
  void** slot = Import(proxy, "WinHttpConnect");
  void* original = slot ? *slot : nullptr;
  if (!slot) return fail("load", "ChsProxyDS imports no WinHttpConnect");

  // The check is about this Windows build, not the user's Microsoft Pinyin
  // settings: with 双拼 on, the extension redirects nothing by design, and the
  // redirect below would fail. Give this disposable process an empty HKCU.
  HKEY blank = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PinyinFlux\\Preflight", 0, nullptr, REG_OPTION_VOLATILE, KEY_READ | KEY_WRITE,
                      nullptr, &blank, nullptr) == ERROR_SUCCESS)
    RegOverridePredefKey(HKEY_CURRENT_USER, blank);

  // Install exactly as in the input host.
  HMODULE module = LoadLibraryW(extension.c_str());
  auto initialize = module ? reinterpret_cast<DWORD (WINAPI*)(void*)>(GetProcAddress(module, "PinyinFluxInitialize")) : nullptr;
  if (!initialize) return fail("extension", "not loadable (" + std::to_string(GetLastError()) + ")");
  if (DWORD started = initialize(nullptr)) return fail("extension", "initialize " + std::to_string(started));
  LONG state = WaitWhile(channel, native::Waiting, 5000);
  if (state != native::Active)
    return fail("hooks", "state " + std::to_string(state) + ", error " + std::to_string(InterlockedCompareExchange(&channel->error, 0, 0)));
  if (channel->triggerSlots <= 0 || channel->networkSlots != 4)
    return fail("hooks", std::to_string(channel->triggerSlots) + " candidate slots, " + std::to_string(channel->networkSlots) + " network slots");

  // A Bing cloud request through the patched imports must reach our listener.
  std::string observed;
  std::thread server([&] {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    timeval timeout{5, 0};
    if (select(0, &set, nullptr, nullptr, &timeout) <= 0) return;
    SOCKET client = accept(listener, nullptr, nullptr);
    DWORD ms = 3000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&ms), sizeof(ms));
    char bytes[4096]{};
    if (int n = recv(client, bytes, sizeof(bytes), 0); n > 0) observed.assign(bytes, n);
    const std::string body = "{\"AS\":{\"Query\":\"nihao\",\"FullResults\":1,\"Results\":[]}}";
    std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body;
    send(client, reply.data(), static_cast<int>(reply.size()), 0);
    closesocket(client);
  });
  Beat(channel);
  auto connect = reinterpret_cast<decltype(&WinHttpConnect)>(*slot);
  void** openSlot = Import(proxy, "WinHttpOpenRequest");
  void** closeSlot = Import(proxy, "WinHttpCloseHandle");
  bool answered = false;
  HINTERNET session = WinHttpOpen(L"PinyinFlux preflight", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
  if (session && openSlot && closeSlot) {
    auto open = reinterpret_cast<decltype(&WinHttpOpenRequest)>(*openSlot);
    auto close = reinterpret_cast<decltype(&WinHttpCloseHandle)>(*closeSlot);
    WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000);
    HINTERNET connection = connect(session, L"cn.bing.com", 443, 0);
    HINTERNET request = connection ? open(connection, L"GET", L"/qsonhs.aspx?q=nihao", nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    if (request && WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
      DWORD status = 0, size = sizeof(status);
      answered = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr) &&
                 status == 200;
    }
    if (request) close(request);
    if (connection) close(connection);
  }
  if (session) WinHttpCloseHandle(session);
  server.join();
  if (!answered || observed.rfind("GET /qsonhs.aspx?q=nihao HTTP/1.1", 0) != 0) return fail("redirect", "cloud request did not reach the core");

  // Stopping restores every patched slot.
  InterlockedExchange(&channel->enabled, 0);
  state = WaitWhile(channel, native::Active, 3000);
  if (state != native::Stopped || *slot != original) return fail("restore", "state " + std::to_string(state));
  // The mapping and modules stay until exit, as in the host.
  Say("PASS hooks, redirect and restore (InputService " + (report.version.empty() ? "?" : report.version) + ")");
  return 0;
}

bool Preflight(const std::wstring& extension, std::string& detail) {
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
  std::wstring command = L"\"" + std::wstring(exe) + L"\" --preflight \"" + extension + L"\"";
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &inherit, 64 * 1024)) { detail = "no pipe"; return false; }
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write;
  startup.hStdError = write;
  PROCESS_INFORMATION process{};
  BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(write);
  if (!started) {
    CloseHandle(read);
    detail = "could not start the check (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  CloseHandle(process.hThread);
  // Read while waiting, so a chatty child can never block on a full pipe.
  std::string output;
  std::thread reader([&] {
    char bytes[1024];
    DWORD n = 0;
    while (ReadFile(read, bytes, sizeof(bytes), &n, nullptr) && n) output.append(bytes, n);
  });
  DWORD exit = 0;
  bool finished = WaitForSingleObject(process.hProcess, kPreflightMs) == WAIT_OBJECT_0;
  if (!finished) TerminateProcess(process.hProcess, ERROR_TIMEOUT);
  WaitForSingleObject(process.hProcess, 5000);
  GetExitCodeProcess(process.hProcess, &exit);
  CloseHandle(process.hProcess);
  reader.join();
  CloseHandle(read);
  size_t end = output.find_first_of("\r\n");
  detail = output.substr(0, end);
  if (!finished) detail = "FAIL timeout: no answer within " + std::to_string(kPreflightMs / 1000) + " s";
  else if (exit != 0 && detail.empty()) detail = "FAIL crashed: " + Hex(exit);
  return finished && exit == 0 && detail.rfind("PASS", 0) == 0;
}

std::string CompatKey(const native::NativeLayout& layout, const std::wstring& extension) {
  // The extension is cached under its SHA-256: ...\inject\<hash>\PinyinFluxInject.dll.
  std::wstring folder = extension.substr(0, extension.find_last_of(L"\\/"));
  std::string hash = WideToUtf8(folder.substr(folder.find_last_of(L"\\/") + 1)).substr(0, 16);
  // The revision of the check itself: a corrected check must not reuse old verdicts.
  // 2: no longer fails while the user has 双拼 on (0.3.2).
  constexpr int kCheckRevision = 2;
  char text[176];
  std::snprintf(text, sizeof(text), "is=%08lx/%lx;px=%08lx/%lx;em=%08lx/%lx;ext=%s;check=%d", layout.engine.image.stamp,
                layout.engine.image.size, layout.proxy.image.stamp, layout.proxy.image.size, layout.legacy.image.stamp,
                layout.legacy.image.size, hash.c_str(), kCheckRevision);
  return text;
}

CompatStore::CompatStore(std::wstring path) : path_(std::move(path)) {
  if (path_.empty()) return;
  // <key> <verdict> <crashes> <detail...>
  std::ifstream in(path_);
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string key, verdict;
    Entry entry;
    if (!(fields >> key >> verdict >> entry.crashes)) continue;
    for (int v = Pass; v <= Crashed; ++v)
      if (verdict == kVerdicts[v]) entry.verdict = static_cast<Verdict>(v);
    std::getline(fields >> std::ws, entry.detail);
    entries_[key] = entry;
  }
}

std::wstring CompatStore::DefaultPath() { return DataDir() + L"\\compat.ini"; }

CompatStore::Entry CompatStore::Get(const std::string& key) const {
  std::lock_guard lock(mutex_);
  auto it = entries_.find(key);
  return it == entries_.end() ? Entry{} : it->second;
}

void CompatStore::Set(const std::string& key, const Entry& entry) {
  std::lock_guard lock(mutex_);
  entries_[key] = entry;
  // Old builds are only worth a few lines.
  while (entries_.size() > kRemembered) entries_.erase(entries_.begin() == entries_.find(key) ? std::next(entries_.begin()) : entries_.begin());
  Save();
}

int CompatStore::Crash(const std::string& key) {
  std::lock_guard lock(mutex_);
  int crashes = ++entries_[key].crashes;
  Save();
  return crashes;
}

void CompatStore::Save() const {
  if (path_.empty()) return;
  std::string text = "# PinyinFlux compatibility checks: <images;extension> <verdict> <host crashes> <detail>\n";
  for (const auto& [key, entry] : entries_)
    text += key + ' ' + kVerdicts[entry.verdict] + ' ' + std::to_string(entry.crashes) + ' ' + entry.detail + '\n';
  std::wstring temp = path_ + L".tmp";
  {
    std::ofstream out(temp, std::ios::trunc | std::ios::binary);
    out << text;
    if (!out) return;
  }
  MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace pf
