// SPDX-License-Identifier: MIT
// Isolated process: verify hooks against the real, pinned Windows images without
// executing their private methods or touching the user's running input method.
#include "../src/PinyinFlux.Core/common.h"
#include "../src/PinyinFlux.Core/native_symbols.h"
#include "../src/PinyinFlux.Inject/protocol.h"
#include <winhttp.h>
#include <cstdio>
#include <thread>
#include <string>
#include <cstring>

using namespace pf::native;
int failures = 0;
void Check(bool ok, const char* message) { printf("%s %s\n", ok ? "PASS" : "FAIL", message); if (!ok) ++failures; }
void** Import(HMODULE module, const char* wanted) {
  auto* base = reinterpret_cast<BYTE*>(module);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  for (; imports->Name; ++imports) {
    if (_stricmp(reinterpret_cast<char*>(base + imports->Name), "winhttp.dll")) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(name->Name, wanted) == 0) return reinterpret_cast<void**>(&slots->u1.Function);
    }
  }
  return nullptr;
}
int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return 2;
  bool unsupported = false, legacy = false, cached = false;
  for (int i = 2; i < argc; ++i) { unsupported |= wcscmp(argv[i], L"--unsupported") == 0; legacy |= wcscmp(argv[i], L"--legacy") == 0; cached |= wcscmp(argv[i], L"--cached") == 0; }
  WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa);
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 1)) return 3;
  int length = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Channel), MappingName(GetCurrentProcessId()).c_str());
  auto* channel = static_cast<Channel*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Channel)));
  if (!channel) return 4;
  new (channel) Channel(); channel->port = ntohs(address.sin_port);
  printf("layout: %s\n", pf::ResolveNativeLayout(channel->layout).c_str());
  InterlockedExchange(&channel->heartbeat, static_cast<LONG>(GetTickCount()));
  wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, ARRAYSIZE(system));
  std::wstring prefix = std::wstring(system) + L"\\InputMethod\\CHS\\";
  HMODULE engine = LoadLibraryExW((legacy ? prefix + L"ChsEM.dll" : std::wstring(system) + L"\\InputService.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  HMODULE proxy = LoadLibraryExW((prefix + L"ChsProxyDS.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  HMODULE extension = LoadLibraryW(argv[1]);
  if (!engine || !proxy) { puts("SKIP Microsoft Pinyin components not available"); return 77; }  // CTest skip code
  Check(extension != nullptr, "load system images and extension into isolated test process");
  if (!extension) return 5;
  auto* head = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(engine) + reinterpret_cast<IMAGE_DOS_HEADER*>(engine)->e_lfanew);
  DWORD savedStamp = head->FileHeader.TimeDateStamp, protect = 0;
  if (unsupported) {
    VirtualProtect(&head->FileHeader.TimeDateStamp, sizeof(DWORD), PAGE_READWRITE, &protect);
    head->FileHeader.TimeDateStamp = 0; // only this test process's private image page
  }
  void** slot = Import(proxy, "WinHttpConnect");
  void* original = slot ? *slot : nullptr;
  HINTERNET session = WinHttpOpen(L"PinyinFlux test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
  WinHttpSetTimeouts(session, 1000, 1000, 1000, 1000);
  HINTERNET cachedConnection = cached ? WinHttpConnect(session, L"cn.bing.com", 443, 0) : nullptr;
  auto initialize = reinterpret_cast<DWORD (WINAPI*)(void*)>(GetProcAddress(extension, "PinyinFluxInitialize"));
  Check(initialize && initialize(nullptr) == ERROR_SUCCESS, "initialize outside loader lock");
  for (int i = 0; i < 50 && channel->state == Waiting; ++i) {
    InterlockedExchange(&channel->heartbeat, static_cast<LONG>(GetTickCount())); Sleep(20);
  }
  if (unsupported) {
    Check(channel->state == Unsupported, "unknown Windows image fails closed");
    Check(channel->triggerSlots == 0 && channel->networkSlots == 0 && *slot == original, "unsupported version installs no hooks");
    head->FileHeader.TimeDateStamp = savedStamp; DWORD ignored;
    VirtualProtect(&head->FileHeader.TimeDateStamp, sizeof(DWORD), protect, &ignored);
  } else {
    Check(channel->state == Active, "validated native candidate + network hooks active");
    if (channel->state != Active) return 6;
    Check(channel->triggerSlots > 0 && channel->networkSlots == 4 && *slot != original, "candidate vtable and all four WinHTTP imports patched");
    std::string observed;
    std::thread server([&] {
      fd_set set; FD_ZERO(&set); FD_SET(listener, &set); timeval timeout{5, 0};
      if (select(0, &set, nullptr, nullptr, &timeout) <= 0) return;
      SOCKET client = accept(listener, nullptr, nullptr);
      DWORD ms = 3000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&ms), sizeof(ms));
      char bytes[4096]{}; int n = recv(client, bytes, sizeof(bytes), 0);
      if (n > 0) observed.assign(bytes, n);
      const std::string body = "{\"AS\":{\"Query\":\"nihao\",\"FullResults\":1,\"Results\":[{\"Suggests\":[{\"Txt\":\"云候选验证\"}]}]}}";
      std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      send(client, reply.data(), static_cast<int>(reply.size()), 0); closesocket(client);
    });
    auto connect = reinterpret_cast<decltype(&WinHttpConnect)>(*slot);
    auto open = reinterpret_cast<decltype(&WinHttpOpenRequest)>(*Import(proxy, "WinHttpOpenRequest"));
    auto option = reinterpret_cast<decltype(&WinHttpSetOption)>(*Import(proxy, "WinHttpSetOption"));
    auto close = reinterpret_cast<decltype(&WinHttpCloseHandle)>(*Import(proxy, "WinHttpCloseHandle"));
    HINTERNET connection = cached ? cachedConnection : connect(session, L"cn.bing.com", 443, 0);
    HINTERNET request = open(connection, L"GET", L"/qsonhs.aspx?q=nihao", nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    wchar_t badProxy[] = L"127.0.0.1:1";
    WINHTTP_PROXY_INFO proxyInfo{WINHTTP_ACCESS_TYPE_NAMED_PROXY, badProxy, nullptr};
    Check(option(request, WINHTTP_OPTION_PROXY, &proxyInfo, sizeof(proxyInfo)) != FALSE, "native proxy override remains direct to loopback");
    bool sent = WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    std::string body;
    if (sent) {
      char bytes[4096]; DWORD read = 0;
      while (WinHttpReadData(request, bytes, sizeof(bytes), &read) && read) body.append(bytes, read);
    }
    close(request); close(connection); WinHttpCloseHandle(session);
    server.join();
    Check(sent && observed.starts_with("GET /qsonhs.aspx?q=nihao HTTP/1.1"), "Bing HTTPS request redirected to local HTTP without a certificate or PAC");
    Check(body.find("云候选验证") != std::string::npos && channel->requests == 1, "cloud payload received through redirected native handles");
    InterlockedExchange(&channel->enabled, 0);
    for (int i = 0; i < 50 && channel->state != Stopped; ++i) Sleep(20);
    Check(channel->state == Stopped && *slot == original, "stop restores original import and candidate vtable slots");
    // A restarted core: the old core's handle closes, so the name now denotes
    // a new section while the extension's old view keeps the old one alive.
    CloseHandle(mapping);
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Channel), MappingName(GetCurrentProcessId()).c_str());
    bool fresh = mapping && GetLastError() != ERROR_ALREADY_EXISTS;
    auto* restarted = mapping ? static_cast<Channel*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Channel))) : nullptr;
    if (!restarted) return 7;
    new (restarted) Channel(); restarted->port = ntohs(address.sin_port);
    restarted->layout = channel->layout;
    InterlockedExchange(&restarted->heartbeat, static_cast<LONG>(GetTickCount()));
    Check(fresh && initialize(nullptr) == ERROR_SUCCESS, "re-initialize the loaded extension for a restarted core");
    for (int i = 0; i < 50 && restarted->state == Waiting; ++i) {
      InterlockedExchange(&restarted->heartbeat, static_cast<LONG>(GetTickCount())); Sleep(20);
    }
    Check(restarted->state == Active && *slot != original, "hooks report to the restarted core's channel");
    InterlockedExchange(&restarted->enabled, 0);
    for (int i = 0; i < 50 && restarted->state != Stopped; ++i) Sleep(20);
    Check(restarted->state == Stopped && *slot == original, "second stop restores original slots");
  }
  closesocket(listener);
  // Retain mapping and modules to mirror the callback lifetime in the real host.
  CloseHandle(mapping); WSACleanup();
  printf("failures=%d\n", failures);
  return failures ? 1 : 0;
}
