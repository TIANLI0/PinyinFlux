#include "native_symbols.h"

#include <dbghelp.h>

#include <cstring>
#include <mutex>

#include "common.h"
#include "http_client.h"
#include "log.h"

namespace pf {

namespace {

std::mutex dbghelpMutex;  // dbghelp is single-threaded

struct Rsds {
  DWORD signature;  // 'RSDS'
  GUID guid;
  DWORD age;
  char name[1];
};

// The build first verified by disassembly (10.0.26100.9767 x64), for when the
// symbol server cannot be reached.
struct Known {
  const wchar_t* image;
  DWORD stamp, size;
  const char* name;
  DWORD rva;
};
constexpr Known kKnown[] = {
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionPinyin::OnProductItemChanged", 0x1083b0},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionPinyin::CheckCOCTriggerRule", 0x106d3c},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionPinyin::CreateCOCPreloadingSuggestionList", 0x2bc30c},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::ProductItemPool::SetCOCExcerptResult", 0x29aa00},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionOfCloud::OnProductItemChanged", 0x10a110},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "CLSID_MtfSuggestionList", 0x412770},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "IID_IMtfSuggestionList", 0x412760},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChsIME::CandidateHandlerConversionOfCloud::OnProductItemChanged", 0x2048b0},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChsIME::CandidateHandlerConversionOfCloud::CheckCOCTriggerSettingRule", 0x203698},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChsIME::CandidateHandlerConversionOfCloud::CheckCOCTriggerCandidateRule", 0x202f08},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionOfCloud::DoCloudQuery", 0x2c0aac},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionOfCloud::RequestSuggestionByString", 0x2c3170},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionOfCloud::OnSuggestionReady", 0x2c1b20},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionOfCloud::FilterSuggestionList", 0x2c0cac},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::CandidateHandlerConversionOfCloud::GetSpellQueryFromResult", 0x2c1768},
    {L"inputservice.dll", 0x78148071, 0x4d9000, "ChxIME::ResultHandlerPinyin::GetCOCExcerptDisplayOrder", 0x2e23f0},
    {L"chsproxyds.dll", 0x34691f04, 0x53000, "ChsQueryHandler::GetCandidates", 0x146e8},
    {L"chsproxyds.dll", 0x34691f04, 0x53000, "ChsQueryHandler::GetCandidatesFromLocalCache", 0x15e20},
    {L"chsproxyds.dll", 0x34691f04, 0x53000, "ChsQueryHandler::AddCloudCandidateData", 0x11154},
    {L"chsproxyds.dll", 0x34691f04, 0x53000, "ChsQueryHandler::AddResponseInfor", 0x11888},
    {L"chsproxyds.dll", 0x34691f04, 0x53000, "ChsQueryHandler::IsExpiredDuringCloudDelay", 0x16120},
    {L"chsproxyds.dll", 0x34691f04, 0x53000, "ChsQueryHandler::s_fCloudTriggerring", 0x4c578},
    {L"chsproxyds.dll", 0x34691f04, 0x53000, "ChsProtocolWrapper::DeserializeResponse", 0x1dd34},
    {L"chsem.dll", 0x2fa8d09e, 0x109000, "ChxIME::CandidateHandlerConversionOfCloud::OnProductItemChanged", 0x6d930},
};

bool WriteWholeFile(const std::wstring& path, const std::string& data) {
  std::wstring temporary = path + L".tmp";
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
  CloseHandle(file);
  return ok && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// x64 instruction shapes used by the field extraction below.
int32_t Disp32(const BYTE* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
bool Rex(BYTE b) { return (b & 0xf0) == 0x40; }
// lea r64, [rcx + disp32]
bool LeaRcx(const BYTE* p) { return (p[0] == 0x48 || p[0] == 0x4c) && p[1] == 0x8d && (p[2] & 0xc7) == 0x81; }

}  // namespace

SymbolImage::~SymbolImage() {
  if (symbols_) {
    std::lock_guard lock(dbghelpMutex);
    if (loadedBase_) SymUnloadModule64(symbols_, loadedBase_);
    SymCleanup(symbols_);
  }
  if (module_) FreeLibrary(module_);
}

bool SymbolImage::Open(const std::wstring& path) {
  path_ = path;
  name_ = path.substr(path.find_last_of(L'\\') + 1);
  for (auto& c : name_) c = static_cast<wchar_t>(towlower(c));
  module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!module_) return false;
  base_ = reinterpret_cast<const BYTE*>(reinterpret_cast<ULONG_PTR>(module_) & ~ULONG_PTR{3});
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base_ + reinterpret_cast<const IMAGE_DOS_HEADER*>(base_)->e_lfanew);
  if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
  id_ = {nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage};
  for (const auto& known : kKnown)
    if (name_ == known.image && id_.stamp == known.stamp && id_.size == known.size) source_ = "built-in";

  // Public PDB: <server>/<pdb>/<GUID><age>/<pdb>, from the CodeView record.
  const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  const Rsds* rsds = nullptr;
  auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(At(directory.VirtualAddress));
  for (DWORD i = 0; entries && i < directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
    if (entries[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW && entries[i].SizeOfData > sizeof(Rsds))
      rsds = reinterpret_cast<const Rsds*>(At(entries[i].AddressOfRawData));
  if (!rsds || rsds->signature != 'SDSR') return true;
  std::string pdb(rsds->name, strnlen(rsds->name, MAX_PATH));
  if (pdb.empty() || pdb.find_first_of("\\/:") != std::string::npos) return true;
  char signature[48];
  const GUID& g = rsds->guid;
  sprintf_s(signature, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX", g.Data1, g.Data2, g.Data3, g.Data4[0],
            g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], rsds->age);
  std::wstring folder = DataDir() + L"\\symbols";
  CreateDirectoryW(folder.c_str(), nullptr);
  folder += L"\\" + Utf8ToWide(pdb);
  CreateDirectoryW(folder.c_str(), nullptr);
  folder += L"\\" + Utf8ToWide(signature);
  CreateDirectoryW(folder.c_str(), nullptr);
  std::wstring local = folder + L"\\" + Utf8ToWide(pdb);
  if (GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES) {
    HttpClient http;
    std::string url = "https://msdl.microsoft.com/download/symbols/" + pdb + "/" + signature + "/" + pdb;
    HttpResponse response = http.Send(L"GET", url, {}, L"", 60000, {}, 64 * 1024 * 1024);
    if (response.status != 200 || response.body.size() < 1024 || !WriteWholeFile(local, response.body)) {
      Log("symbols: %s unavailable (HTTP %d)", pdb.c_str(), response.status);
      return true;
    }
    Log("symbols: downloaded %s (%zu KB)", pdb.c_str(), response.body.size() / 1024);
  }
  std::lock_guard lock(dbghelpMutex);
  symbols_ = reinterpret_cast<HANDLE>(this);  // any unique value; no process is read
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_EXACT_SYMBOLS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
  if (!SymInitializeW(symbols_, folder.c_str(), FALSE)) { symbols_ = nullptr; return true; }
  loadedBase_ = SymLoadModuleExW(symbols_, nullptr, path.c_str(), nullptr, 0x10000000, id_.size, nullptr, 0);
  IMAGEHLP_MODULEW64 info{sizeof(info)};
  if (loadedBase_ && SymGetModuleInfoW64(symbols_, loadedBase_, &info) && info.SymType == SymPdb) source_ = "symbols";
  else Log("symbols: %s present but not loadable", pdb.c_str());
  return true;
}

DWORD SymbolImage::Rva(const char* name) {
  if (source_ == "symbols") {
    std::lock_guard lock(dbghelpMutex);
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    if (!SymFromName(symbols_, name, symbol) || symbol->Address <= loadedBase_) return 0;
    DWORD64 rva = symbol->Address - loadedBase_;
    return rva < id_.size ? static_cast<DWORD>(rva) : 0;
  }
  for (const auto& known : kKnown)
    if (name_ == known.image && id_.stamp == known.stamp && id_.size == known.size && std::strcmp(name, known.name) == 0)
      return known.rva;
  return 0;
}

bool SymbolImage::Fill(native::Function& function, const char* name) {
  function = {};
  DWORD rva = Rva(name);
  if (!rva || rva + native::kEntryBytes > id_.size) return false;
  function.rva = rva;
  std::memcpy(function.entry, base_ + rva, native::kEntryBytes);
  return true;
}

namespace {

// Field offsets are read from the instructions that use them, so a build
// that rearranges a class still resolves (or is rejected) on its own terms.
struct Code {
  const BYTE* begin;
  size_t size;
};
Code Body(const SymbolImage& image, DWORD rva, size_t size) {
  const BYTE* p = image.At(rva);
  if (!p || rva + size > image.Id().size) return {nullptr, 0};
  return {p, size};
}

bool EngineFields(SymbolImage& image, native::EngineLayout& e) {
  // SetCOCExcerptResult: lea r14, [rcx + excerpts]
  Code excerpt = Body(image, e.setExcerpts.rva, 0x40);
  for (size_t i = 0; excerpt.begin && i + 7 <= excerpt.size && !e.excerpts; ++i)
    if (LeaRcx(excerpt.begin + i) && Disp32(excerpt.begin + i + 3) > 0 && Disp32(excerpt.begin + i + 3) < 0x2000)
      e.excerpts = static_cast<DWORD>(Disp32(excerpt.begin + i + 3));

  // CheckCOCTriggerRule: cmp byte ptr [rdi + flag], r15b (x3), then calls
  // through mov rcx, [rdi + settings]; mov rax, [rcx]; mov rax, [rax + slot].
  Code rule = Body(image, e.triggerRule.rva, 0x900);
  size_t flags = 0, afterFlags = 0, slots = 0;
  for (size_t i = 0; rule.begin && i + 7 <= rule.size; ++i) {
    const BYTE* p = rule.begin + i;
    if (flags < 3 && Rex(p[0]) && p[1] == 0x38 && p[2] == 0xbf) {
      e.ruleFlags[flags++] = static_cast<DWORD>(Disp32(p + 3));
      afterFlags = i + 7;
    } else if (!e.ruleSettings && p[0] == 0x48 && p[1] == 0x8b && p[2] == 0x8f) {
      e.ruleSettings = static_cast<DWORD>(Disp32(p + 3));
    } else if (flags == 3 && i >= afterFlags && slots < 4 && p[0] == 0x48 && p[1] == 0x8b && p[2] == 0x80) {
      e.settingsSlots[slots++] = static_cast<DWORD>(Disp32(p + 3));
    } else if (!e.candidateText && p[0] == 0x0f && p[1] == 0xb7 && p[2] == 0x50 && p[3] && p[3] < 0x40) {
      // The rule reads its first local candidate: movzx edx, word ptr [rax + text].
      e.candidateText = p[3];
    }
  }

  // OnProductItemChanged (pinyin handler interface): lea r14, [rcx - base],
  // then the spelling lookup: mov rcx, [rsi + pool]; ...; mov edx, 8; mov rax, [rax + slot].
  Code changed = Body(image, e.pinyinChanged.rva, 0x200);
  int32_t base = 0;
  for (size_t i = 0; changed.begin && i + 7 <= changed.size && !base; ++i)
    if (LeaRcx(changed.begin + i) && Disp32(changed.begin + i + 3) < 0 && Disp32(changed.begin + i + 3) > -0x1000)
      base = -Disp32(changed.begin + i + 3);
  Code spelling = Body(image, e.pinyinChanged.rva, 0x1000);
  for (size_t i = 20; base && spelling.begin && i + 9 <= spelling.size && !e.poolSpelling; ++i) {
    const BYTE* p = spelling.begin + i;
    if (p[0] != 0xba || Disp32(p + 1) != 8 || p[5] != 0x48 || p[6] != 0x8b || p[7] != 0x40) continue;
    for (size_t back = 4; back <= 20; ++back) {
      const BYTE* q = p - back;
      if (q[0] == 0x48 && q[1] == 0x8b && q[2] == 0x4e) {
        e.rulePool = static_cast<DWORD>(base + q[3]);
        e.poolSpelling = p[8];
        break;
      }
    }
  }

  // ChsIME cloud handler interface: lea rax, [rdi - base]; ...; neg rax (null-checked cast).
  Code cloud = Body(image, e.cloudChanged.rva, 0x300);
  for (size_t i = 0; cloud.begin && i + 18 <= cloud.size && !e.cloudBase; ++i) {
    const BYTE* p = cloud.begin + i;
    if (p[0] != 0x48 || p[1] != 0x8d || p[2] != 0x87 || Disp32(p + 3) >= 0 || Disp32(p + 3) <= -0x1000) continue;
    for (size_t gap = 7; gap <= 15; ++gap)
      if (p[gap] == 0x48 && p[gap + 1] == 0xf7 && p[gap + 2] == 0xd8) e.cloudBase = static_cast<DWORD>(-Disp32(p + 3));
  }

  // The extension replays notification bits 0x8 (regenerate candidates),
  // 0x1000 (cloud result arrived) and 0x400 (fill the placeholder); the
  // handler must still test them: bt reg32, 0Ch / 0Ah and test reg8, 8.
  Code handler = Body(image, e.pinyinChanged.rva, 0x1000);
  bool arrival = false, fill = false, regenerate = false;
  for (size_t i = 0; handler.begin && i + 5 <= handler.size; ++i) {
    const BYTE* p = handler.begin + i;
    const BYTE* q = Rex(p[0]) ? p + 1 : p;
    if (q[0] == 0x0f && q[1] == 0xba && (q[2] & 0xf8) == 0xe0) { arrival |= q[3] == 0x0c; fill |= q[3] == 0x0a; }
    if (q[0] == 0xf6 && (q[1] & 0xf8) == 0xc0 && q[2] == 0x08) regenerate = true;
  }

  auto field = [](DWORD v) { return v && v < 0x1000; };
  auto slot = [](DWORD v) { return v && v < 0x800 && v % 8 == 0; };
  return arrival && fill && regenerate && field(e.excerpts) && flags == 3 && field(e.ruleFlags[0]) && field(e.ruleFlags[1]) && field(e.ruleFlags[2]) &&
         field(e.ruleSettings) && slots == 4 && slot(e.settingsSlots[0]) && slot(e.settingsSlots[1]) &&
         slot(e.settingsSlots[2]) && slot(e.settingsSlots[3]) && field(e.rulePool) && slot(e.poolSpelling);
}

std::string Describe(const SymbolImage& image, bool complete) {
  std::string s = image.Source().empty() ? "no symbols" : image.Source();
  return complete ? s : s + ", incomplete";
}

}  // namespace

std::string ResolveNativeLayout(native::NativeLayout& layout) {
  layout = {};
  wchar_t buffer[MAX_PATH]{};
  GetSystemDirectoryW(buffer, MAX_PATH);
  const std::wstring system = buffer;
  std::string summary;

  SymbolImage engine;
  if (engine.Open(system + L"\\InputService.dll")) {
    auto& e = layout.engine;
    e.image = engine.Id();
    bool complete = engine.Fill(e.pinyinChanged, "ChxIME::CandidateHandlerConversionPinyin::OnProductItemChanged") &
        engine.Fill(e.triggerRule, "ChxIME::CandidateHandlerConversionPinyin::CheckCOCTriggerRule") &
        engine.Fill(e.preloading, "ChxIME::CandidateHandlerConversionPinyin::CreateCOCPreloadingSuggestionList") &
        engine.Fill(e.setExcerpts, "ChxIME::ProductItemPool::SetCOCExcerptResult") &
        engine.Fill(e.desktopChanged, "ChxIME::CandidateHandlerConversionOfCloud::OnProductItemChanged");
    e.listClass = engine.Rva("CLSID_MtfSuggestionList");
    e.listInterface = engine.Rva("IID_IMtfSuggestionList");
    engine.Fill(e.cloudChanged, "ChsIME::CandidateHandlerConversionOfCloud::OnProductItemChanged");
    engine.Fill(e.settingRule, "ChsIME::CandidateHandlerConversionOfCloud::CheckCOCTriggerSettingRule");
    engine.Fill(e.candidateRule, "ChsIME::CandidateHandlerConversionOfCloud::CheckCOCTriggerCandidateRule");
    engine.Fill(e.doCloudQuery, "ChxIME::CandidateHandlerConversionOfCloud::DoCloudQuery");
    engine.Fill(e.requestSuggestion, "ChxIME::CandidateHandlerConversionOfCloud::RequestSuggestionByString");
    engine.Fill(e.suggestionReady, "ChxIME::CandidateHandlerConversionOfCloud::OnSuggestionReady");
    engine.Fill(e.filterSuggestions, "ChxIME::CandidateHandlerConversionOfCloud::FilterSuggestionList");
    engine.Fill(e.spellQuery, "ChxIME::CandidateHandlerConversionOfCloud::GetSpellQueryFromResult");
    engine.Fill(e.displayOrder, "ChxIME::ResultHandlerPinyin::GetCOCExcerptDisplayOrder");
    complete = complete && e.listClass && e.listInterface && EngineFields(engine, e);
    if (!complete) e.pinyinChanged.rva = 0;  // the extension then reports Unsupported
    summary += "InputService " + Describe(engine, complete);
    Log("native layout: InputService %08lx/%lx %s", e.image.stamp, e.image.size, Describe(engine, complete).c_str());
  }

  SymbolImage proxy;
  if (proxy.Open(system + L"\\InputMethod\\CHS\\ChsProxyDS.dll")) {
    auto& p = layout.proxy;
    p.image = proxy.Id();
    proxy.Fill(p.getCandidates, "ChsQueryHandler::GetCandidates");
    proxy.Fill(p.localCache, "ChsQueryHandler::GetCandidatesFromLocalCache");
    proxy.Fill(p.addCloud, "ChsQueryHandler::AddCloudCandidateData");
    proxy.Fill(p.addResponse, "ChsQueryHandler::AddResponseInfor");
    // The debounce is replaced only together with the lock it takes:
    // cmp byte ptr [rip + lock], 0 inside IsExpiredDuringCloudDelay.
    if (proxy.Fill(p.debounce, "ChsQueryHandler::IsExpiredDuringCloudDelay")) {
      DWORD lock = proxy.Rva("ChsQueryHandler::s_fCloudTriggerring");
      Code body = Body(proxy, p.debounce.rva, 0x100);
      for (size_t i = 0; lock && body.begin && i + 7 <= body.size && !p.requestLock; ++i)
        if (body.begin[i] == 0x80 && body.begin[i + 1] == 0x3d && body.begin[i + 6] == 0x00 &&
            p.debounce.rva + i + 7 + Disp32(body.begin + i + 2) == lock)
          p.requestLock = lock;
      if (!p.requestLock) p.debounce = {};
    }
    bool complete = p.localCache.rva && p.addResponse.rva && p.debounce.rva;
    summary += "; ChsProxyDS " + Describe(proxy, complete);
    Log("native layout: ChsProxyDS %08lx/%lx %s", p.image.stamp, p.image.size, Describe(proxy, complete).c_str());
  }

  SymbolImage legacy;
  if (legacy.Open(system + L"\\InputMethod\\CHS\\ChsEM.dll")) {
    layout.legacy.image = legacy.Id();
    legacy.Fill(layout.legacy.cloudChanged, "ChxIME::CandidateHandlerConversionOfCloud::OnProductItemChanged");
  }
  return summary;
}

}  // namespace pf
