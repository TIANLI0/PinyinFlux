// SPDX-License-Identifier: MIT
// Verify the real Microsoft response parser in an isolated process. Its private
// ABI is used only for this pinned test, never from the running input method.
#include "../src/PinyinFlux.Core/common.h"
#include "../src/PinyinFlux.Core/bing_format.h"
#include "../src/PinyinFlux.Core/cloud_query.h"
#include "../src/PinyinFlux.Core/json.h"
#include "../src/PinyinFlux.Core/native_symbols.h"
#include <roapi.h>
#include <cstdio>

struct NativeString {
  union { wchar_t inlineText[8]; wchar_t* pointer; } data;
  size_t size, capacity;
  std::wstring Read() const { return size > 4096 ? L"" : std::wstring(capacity >= 8 ? data.pointer : data.inlineText, size); }
};
struct NativePair { NativeString first, second; };
struct NativeVector { NativePair* begin = nullptr; NativePair* end = nullptr; NativePair* capacity = nullptr; };
static_assert(sizeof(NativeString) == 32 && sizeof(std::wstring) == 32 && sizeof(NativePair) == 64);
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  struct Fixture { const char* input; const char* expected; };
  const Fixture fixtures[] = {
    {"JueQuLing", "juequling"},
    {"jue qu ling", "jue'qu'ling"},
    {"{_t_:1,_cl_:_w_,_q_:_juequling_,_app_:_Example.exe_,_c_:5}", "juequling"},
    {"{_q_:_ni'hao_}", "ni'hao"},
    {"{\"q\":\"NI HAO\",\"app\":\"Example.exe\"}", "ni'hao"},
    {"{_app_:_juequling.exe_}", ""},
    {"{_q_:_nihao_,_q_:_world_}", ""},
    {"{_q_:_你好_}", ""},
    {"{_q_:_nihao_", ""},
    {"http://example.com", ""}
  };
  for (const auto& fixture : fixtures) {
    if (pf::ExtractCloudPinyin(fixture.input) != fixture.expected) { puts("FAIL native request envelope decoding"); return 4; }
  }
  puts("PASS native request envelopes, separators and invalid inputs");
  RoInitialize(RO_INIT_MULTITHREADED);
  wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, ARRAYSIZE(system));
  std::wstring path = std::wstring(system) + L"\\InputMethod\\CHS\\ChsProxyDS.dll";
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  // CTest skip code: the Chinese IME is not installed (e.g. Windows Server CI images).
  if (!module) { printf("SKIP ChsProxyDS.dll not available (%lu)\n", GetLastError()); return 77; }
  auto* base = reinterpret_cast<BYTE*>(module);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  // The query handler context built below (spelling at +0x1a8) is verified for this build only.
  if (nt->FileHeader.TimeDateStamp != 0x34691f04 || nt->OptionalHeader.SizeOfImage != 0x53000) {
    puts("SKIP native record builder: private context verified for ChsProxyDS 34691f04 only"); return 0;
  }
  pf::SymbolImage symbols;
  DWORD parseRva = symbols.Open(path) ? symbols.Rva("ChsProtocolWrapper::DeserializeResponse") : 0;
  DWORD addRva = parseRva ? symbols.Rva("ChsQueryHandler::AddResponseInfor") : 0;
  if (!parseRva || !addRva) { puts("FAIL resolving native parser symbols"); return 2; }
  using Parse = HRESULT (WINAPI*)(const std::wstring&, NativeVector&);
  auto parse = reinterpret_cast<Parse>(base + parseRva);
  NativeVector convertedFields;
  std::vector<pf::Candidate> candidates = {{"绝区零", 9, true, "jue'qu'ling"}};
  auto conversionJson = pf::Utf8ToWide(pf::BuildNativeBingResponse("juequling", "juequling", candidates));
  HRESULT conversion = parse(conversionJson, convertedFields);
  printf("conversion fields HRESULT=0x%08lx fields=%td\n", static_cast<unsigned long>(conversion), convertedFields.end - convertedFields.begin);
  if (FAILED(conversion) || convertedFields.end - convertedFields.begin < 5) return 5;
  alignas(16) BYTE context[0x300]{};
  new (context + 0x1a8) std::wstring(L"juequling");
  struct NativeRecords { BYTE* begin = nullptr; BYTE* end = nullptr; BYTE* capacity = nullptr; } records;
  using Add = HRESULT (WINAPI*)(void*, const NativeVector&, NativeRecords&);
  auto add = reinterpret_cast<Add>(base + addRva);
  HRESULT added = add(context, convertedFields, records);
  printf("native conversion HRESULT=0x%08lx bytes=%td\n", static_cast<unsigned long>(added), records.end - records.begin);
  if (FAILED(added) || !records.begin || records.end - records.begin != 0xf8) return 6;
  bool hasWord = false, hasReading = false;
  for (size_t offset = 0; offset < 0x80; offset += 0x20) {
    const auto* item = reinterpret_cast<const NativeString*>(records.begin + offset);
    if (item->size > item->capacity || item->capacity > 4096) continue;
    auto value = item->Read();
    printf("native conversion field %zu=%s\n", offset, pf::WideToUtf8(value).c_str());
    hasWord |= value == L"绝区零";
    hasReading |= value.find(L"jue") != std::wstring::npos;
  }
  printf("native conversion kind=%u\n", *reinterpret_cast<WORD*>(records.begin + 0xec));
  if (!hasWord || !hasReading || *reinterpret_cast<WORD*>(records.begin + 0xec) != 1) return 7;
  // Abbreviated input: the record keeps the typed spelling and the full reading.
  NativeVector abbreviatedFields;
  std::vector<pf::Candidate> abbreviated = {{"原神启动", 14, true, "yuan'shen'qi'dong"}};
  auto abbreviatedJson = pf::Utf8ToWide(pf::BuildNativeBingResponse("yuanshenqd", "yuanshenqd", abbreviated));
  if (FAILED(parse(abbreviatedJson, abbreviatedFields))) return 8;
  alignas(16) BYTE abbreviatedContext[0x300]{};
  new (abbreviatedContext + 0x1a8) std::wstring(L"yuanshenqd");
  NativeRecords abbreviatedRecords;
  if (FAILED(add(abbreviatedContext, abbreviatedFields, abbreviatedRecords)) || abbreviatedRecords.end - abbreviatedRecords.begin != 0xf8 ||
      reinterpret_cast<const NativeString*>(abbreviatedRecords.begin)->Read() != L"yuanshenqd" ||
      reinterpret_cast<const NativeString*>(abbreviatedRecords.begin + 0x20)->Read() != L"原神启动" ||
      reinterpret_cast<const NativeString*>(abbreviatedRecords.begin + 0x60)->Read() != L"yuan shen qi dong") return 9;
  puts("PASS abbreviated input builds a native conversion record with the full reading");
  NativeVector result;
  auto json = pf::Utf8ToWide(pf::BuildBingResponse("juequling", {"绝区零", "测试云候选"}));
  HRESULT hr = parse(json, result);
  printf("native parser HRESULT=0x%08lx candidates=%td\n", static_cast<unsigned long>(hr), result.end - result.begin);
  bool found = false;
  if (SUCCEEDED(hr) && result.begin && result.end >= result.begin && result.end - result.begin <= 32) {
    for (auto* pair = result.begin; pair != result.end; ++pair) {
      auto first = pair->first.Read(), second = pair->second.Read();
      printf("candidate first=%s second=%s\n", pf::WideToUtf8(first).c_str(), pf::WideToUtf8(second).c_str());
      found |= first.find(L"绝区零") != std::wstring::npos || second.find(L"绝区零") != std::wstring::npos;
    }
  }
  // Native parser allocations deliberately live until this short test process
  // exits; never free MSVCRT-owned objects with the executable's static CRT.
  return SUCCEEDED(hr) && found ? 0 : 3;
}
