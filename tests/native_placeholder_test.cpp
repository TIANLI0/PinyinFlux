// SPDX-License-Identifier: MIT
// Exercise the real COM suggestion-list factory and the installed no-placeholder
// detour in an isolated process; no desktop interaction or running IME access.
#include "../src/PinyinFlux.Core/native_symbols.h"
#include "../src/PinyinFlux.Inject/protocol.h"
#include <roapi.h>
#include <cstdio>
using namespace pf::native;
int wmain(int argc, wchar_t** argv) {
  if (argc != 2 || FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
  wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, ARRAYSIZE(system));
  HMODULE engine = LoadLibraryExW((std::wstring(system) + L"\\InputService.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!engine) { puts("SKIP InputService.dll not available"); return 77; }  // CTest skip code
  auto* base = reinterpret_cast<BYTE*>(engine);
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Channel), MappingName(GetCurrentProcessId()).c_str());
  auto* channel = static_cast<Channel*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Channel)));
  if (!channel) return 4;
  new (channel) Channel();
  printf("layout: %s\n", pf::ResolveNativeLayout(channel->layout).c_str());
  if (!channel->layout.engine.pinyinChanged.rva) return 3;
  InterlockedExchange(&channel->heartbeat, static_cast<LONG>(GetTickCount()));
  HMODULE extension = LoadLibraryW(argv[1]);
  auto initialize = reinterpret_cast<DWORD (WINAPI*)(void*)>(GetProcAddress(extension, "PinyinFluxInitialize"));
  if (!initialize || initialize(nullptr)) return 5;
  for (int i = 0; i < 100 && channel->triggerSlots == 0; ++i) Sleep(20);
  if (channel->triggerSlots == 0 || channel->state == Failed || channel->state == Unsupported) return 6;
  // triggerSlots is published slightly before the worker sets live.
  Sleep(150);
  using Preload = HRESULT (WINAPI*)(void*, void**);
  void* list = nullptr;
  auto create = reinterpret_cast<Preload>(base + channel->layout.engine.preloading.rva);
  HRESULT hr = create(nullptr, &list);
  if (FAILED(hr) || !list) { printf("factory HRESULT=%08lx\n", hr); return 7; }
  using Count = HRESULT (WINAPI*)(void*, DWORD*);
  auto** table = *static_cast<void***>(list);
  DWORD count = 99;
  if (FAILED(reinterpret_cast<Count>(table[3])(list, &count)) || count != 0) return 8;
  static_cast<IUnknown*>(list)->Release();
  puts("PASS real native preloading list contains zero blank candidates");
  InterlockedExchange(&channel->enabled, 0);
  for (int i = 0; i < 100 && channel->state != Stopped; ++i) Sleep(20);
  return channel->state == Stopped ? 0 : 9;
}
