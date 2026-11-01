#include "config.h"

#include <atomic>

#include "common.h"

namespace pf {

namespace {
std::atomic<std::shared_ptr<const Config>> g_current{std::make_shared<const Config>()};

std::string ReadString(const wchar_t* section, const wchar_t* key, const std::string& fallback) {
  wchar_t buf[1024];
  DWORD n = GetPrivateProfileStringW(section, key, L"\x1", buf, ARRAYSIZE(buf), ConfigPath().c_str());
  if (n == 1 && buf[0] == L'\x1') return fallback;
  return WideToUtf8(std::wstring_view(buf, n));
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback, int lo, int hi) {
  int v = static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ConfigPath().c_str()));
  return v < lo ? lo : (v > hi ? hi : v);
}

void WriteString(const wchar_t* section, const wchar_t* key, const std::string& value) {
  WritePrivateProfileStringW(section, key, Utf8ToWide(value).c_str(), ConfigPath().c_str());
}

void WriteInt(const wchar_t* section, const wchar_t* key, int value) {
  WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), ConfigPath().c_str());
}
}  // namespace

std::wstring ConfigPath() { return DataDir() + L"\\config.ini"; }

std::shared_ptr<const Config> LoadConfig() {
  auto c = std::make_shared<Config>();
  const Config d;
  c->port = ReadInt(L"Core", L"Port", d.port, 1024, 65535);
  c->nativeInjection = ReadInt(L"Core", L"NativeInjection", d.nativeInjection, 0, 1) != 0;
  c->timeoutMs = ReadInt(L"Core", L"TimeoutMs", d.timeoutMs, 100, 5000);
  c->maxCandidates = ReadInt(L"Core", L"MaxCandidates", d.maxCandidates, 1, 10);
  c->imeOnly = ReadInt(L"Core", L"ImeOnly", d.imeOnly, 0, 1) != 0;
  c->logToFile = ReadInt(L"Core", L"Log", d.logToFile, 0, 1) != 0;
  c->reclaimProxy = ReadInt(L"Core", L"ReclaimProxy", d.reclaimProxy, 0, 1) != 0;
  c->typingStats = ReadInt(L"Core", L"TypingStats", d.typingStats, 0, 1) != 0;

  std::string providers = ReadString(L"Core", L"Providers", "");
  if (!providers.empty()) {
    auto list = Split(ToLowerAscii(providers), ',');
    if (!list.empty()) c->providers = std::move(list);
  }

  c->baiduUrl = ReadString(L"Baidu", L"Url", d.baiduUrl);
  c->sogouUrl = ReadString(L"Sogou", L"Url", d.sogouUrl);
  c->sogouIp = std::string(Trim(ReadString(L"Sogou", L"Ip", d.sogouIp)));
  return c;
}

void SaveConfig(const Config& c) {
  WriteInt(L"Core", L"Port", c.port);
  WriteInt(L"Core", L"NativeInjection", c.nativeInjection ? 1 : 0);
  WriteInt(L"Core", L"TimeoutMs", c.timeoutMs);
  WriteInt(L"Core", L"MaxCandidates", c.maxCandidates);
  WriteInt(L"Core", L"ImeOnly", c.imeOnly ? 1 : 0);
  WriteInt(L"Core", L"Log", c.logToFile ? 1 : 0);
  WriteInt(L"Core", L"ReclaimProxy", c.reclaimProxy ? 1 : 0);
  WriteInt(L"Core", L"TypingStats", c.typingStats ? 1 : 0);
  std::string providers;
  for (const auto& p : c.providers) {
    if (!providers.empty()) providers += ',';
    providers += p;
  }
  WriteString(L"Core", L"Providers", providers);
  WriteString(L"Baidu", L"Url", c.baiduUrl);
  WriteString(L"Sogou", L"Url", c.sogouUrl);
  WriteString(L"Sogou", L"Ip", c.sogouIp);
}

std::shared_ptr<const Config> CurrentConfig() { return g_current.load(); }

void SetCurrentConfig(std::shared_ptr<const Config> config) { g_current.store(std::move(config)); }

}  // namespace pf
