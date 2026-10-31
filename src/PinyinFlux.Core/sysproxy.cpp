#include "sysproxy.h"

#include "common.h"

#include <wininet.h>

#include <mutex>

#include "http_client.h"
#include "log.h"

namespace pf {

namespace {

// wininet.dll (plus iertutil) is only needed for the occasional settings
// change, so it is loaded on demand and released right after.
class WinInet {
 public:
  WinInet() : module_(LoadLibraryExW(L"wininet.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    if (module_) {
      query_ = reinterpret_cast<decltype(query_)>(GetProcAddress(module_, "InternetQueryOptionW"));
      set_ = reinterpret_cast<decltype(set_)>(GetProcAddress(module_, "InternetSetOptionW"));
    }
  }
  ~WinInet() {
    if (module_) FreeLibrary(module_);
  }
  WinInet(const WinInet&) = delete;
  WinInet& operator=(const WinInet&) = delete;

  BOOL Query(DWORD option, void* buffer, DWORD* size) const {
    return query_ ? query_(nullptr, option, buffer, size) : FALSE;
  }
  BOOL Set(DWORD option, void* buffer, DWORD size) const { return set_ ? set_(nullptr, option, buffer, size) : FALSE; }

 private:
  HMODULE module_;
  BOOL(WINAPI* query_)(HINTERNET, DWORD, LPVOID, LPDWORD) = nullptr;
  BOOL(WINAPI* set_)(HINTERNET, DWORD, LPVOID, DWORD) = nullptr;
};

// The settings that were active before PinyinFlux, persisted so a crash or
// reboot never loses them.
struct PreviousProxy {
  DWORD flags = PROXY_TYPE_DIRECT;
  std::wstring server;
  std::wstring bypass;
  std::wstring autoConfigUrl;
};

std::wstring StatePath() { return DataDir() + L"\\proxy-state.ini"; }

void SavePrevious(const PreviousProxy& p) {
  auto path = StatePath();
  WritePrivateProfileStringW(L"Previous", L"Flags", std::to_wstring(p.flags).c_str(), path.c_str());
  WritePrivateProfileStringW(L"Previous", L"Server", p.server.c_str(), path.c_str());
  WritePrivateProfileStringW(L"Previous", L"Bypass", p.bypass.c_str(), path.c_str());
  WritePrivateProfileStringW(L"Previous", L"AutoConfigUrl", p.autoConfigUrl.c_str(), path.c_str());
}

bool LoadPrevious(PreviousProxy& p) {
  auto path = StatePath();
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  wchar_t buf[2048];
  p.flags = GetPrivateProfileIntW(L"Previous", L"Flags", PROXY_TYPE_DIRECT, path.c_str());
  GetPrivateProfileStringW(L"Previous", L"Server", L"", buf, ARRAYSIZE(buf), path.c_str());
  p.server = buf;
  GetPrivateProfileStringW(L"Previous", L"Bypass", L"", buf, ARRAYSIZE(buf), path.c_str());
  p.bypass = buf;
  GetPrivateProfileStringW(L"Previous", L"AutoConfigUrl", L"", buf, ARRAYSIZE(buf), path.c_str());
  p.autoConfigUrl = buf;
  return true;
}

bool QueryCurrent(PreviousProxy& p) {
  INTERNET_PER_CONN_OPTIONW opts[4] = {};
  opts[0].dwOption = INTERNET_PER_CONN_FLAGS;
  opts[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
  opts[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
  opts[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
  INTERNET_PER_CONN_OPTION_LISTW list{sizeof(list), nullptr, ARRAYSIZE(opts), 0, opts};
  DWORD size = sizeof(list);
  if (!WinInet().Query(INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size)) return false;
  p.flags = opts[0].Value.dwValue;
  auto take = [](LPWSTR s) {
    std::wstring v = s ? s : L"";
    if (s) GlobalFree(s);
    return v;
  };
  p.server = take(opts[1].Value.pszValue);
  p.bypass = take(opts[2].Value.pszValue);
  p.autoConfigUrl = take(opts[3].Value.pszValue);
  return true;
}

bool SetSettings(DWORD flags, const std::wstring& server, const std::wstring& bypass, const std::wstring& pacUrl) {
  INTERNET_PER_CONN_OPTIONW opts[4] = {};
  opts[0].dwOption = INTERNET_PER_CONN_FLAGS;
  opts[0].Value.dwValue = flags;
  opts[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
  opts[1].Value.pszValue = const_cast<LPWSTR>(server.c_str());
  opts[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
  opts[2].Value.pszValue = const_cast<LPWSTR>(bypass.c_str());
  opts[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
  opts[3].Value.pszValue = const_cast<LPWSTR>(pacUrl.c_str());
  INTERNET_PER_CONN_OPTION_LISTW list{sizeof(list), nullptr, ARRAYSIZE(opts), 0, opts};
  WinInet inet;
  if (!inet.Set(INTERNET_OPTION_PER_CONNECTION_OPTION, &list, sizeof(list))) {
    Log("proxy: InternetSetOption failed %lu", GetLastError());
    return false;
  }
  inet.Set(INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
  inet.Set(INTERNET_OPTION_REFRESH, nullptr, 0);
  return true;
}

bool IsOurPacUrl(const std::wstring& url) { return url.find(L"/pinyinflux.pac") != std::wstring::npos; }

std::string JsString(const std::wstring& s) {
  std::string out = "\"";
  for (char c : WideToUtf8(s)) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + '"';
}

// Converts WinINET's manual proxy + bypass list into PAC logic.
std::string ManualProxyPac(const PreviousProxy& p) {
  std::string server = WideToUtf8(p.server);
  // "http=host:port;https=host:port" or plain "host:port".
  std::string httpsProxy, anyProxy;
  for (const auto& part : Split(server, ';')) {
    size_t eq = part.find('=');
    if (eq == std::string::npos) anyProxy = part;
    else if (IEquals(part.substr(0, eq), "https")) httpsProxy = part.substr(eq + 1);
    else if (IEquals(part.substr(0, eq), "http") && anyProxy.empty()) anyProxy = part.substr(eq + 1);
  }
  std::string target = !httpsProxy.empty() ? httpsProxy : anyProxy;
  if (target.empty()) return "  return \"DIRECT\";\n";

  std::string js;
  for (const auto& entry : Split(WideToUtf8(p.bypass), ';')) {
    if (entry == "<local>") {
      js += "  if (isPlainHostName(host)) return \"DIRECT\";\n";
    } else {
      js += "  if (shExpMatch(host, " + JsString(Utf8ToWide(entry)) + ")) return \"DIRECT\";\n";
    }
  }
  js += "  return \"PROXY " + target + "; DIRECT\";\n";
  return js;
}

// A previous PAC script is embedded with its entry point renamed. Cached; the
// fetch happens at most once per URL per process.
std::string PreviousPacBody(const std::wstring& url) {
  static std::mutex mutex;
  static std::wstring cachedUrl;
  static std::string cachedBody;
  std::lock_guard lock(mutex);
  if (url == cachedUrl) return cachedBody;
  cachedUrl = url;
  cachedBody.clear();
  if (IStartsWith(WideToUtf8(url), "http")) {
    HttpClient http;
    HttpResponse r = http.Send(L"GET", WideToUtf8(url), {}, {}, 3000);
    if (r.status == 200) cachedBody = r.body;
  }
  if (cachedBody.empty()) Log("proxy: could not fetch previous PAC; non-Bing traffic will go DIRECT");
  return cachedBody;
}

}  // namespace

std::wstring PacUrl(int port) { return L"http://127.0.0.1:" + std::to_wstring(port) + L"/pinyinflux.pac"; }

std::string BuildPacScript(int port) {
  PreviousProxy prev;
  LoadPrevious(prev);

  std::string pac;
  std::string fallback = "  return \"DIRECT\";\n";
  if ((prev.flags & PROXY_TYPE_AUTO_PROXY_URL) && !prev.autoConfigUrl.empty() && !IsOurPacUrl(prev.autoConfigUrl)) {
    std::string body = PreviousPacBody(prev.autoConfigUrl);
    size_t at = body.find("FindProxyForURL");
    if (at != std::string::npos) {
      body.replace(at, strlen("FindProxyForURL"), "__pinyinflux_previous_FindProxyForURL");
      pac += body + "\n";
      fallback = "  return __pinyinflux_previous_FindProxyForURL(url, host);\n";
    }
  } else if (prev.flags & PROXY_TYPE_PROXY) {
    fallback = ManualProxyPac(prev);
  }

  pac += "function FindProxyForURL(url, host) {\n"
         "  var h = host.toLowerCase();\n"
         "  if (h == \"cn.bing.com\" || h == \"www.bing.com\")\n"
         "    return \"PROXY 127.0.0.1:" + std::to_string(port) + "; DIRECT\";\n" +
         fallback + "}\n";
  return pac;
}

bool IsSystemProxyApplied(int port) {
  PreviousProxy cur;
  return QueryCurrent(cur) && (cur.flags & PROXY_TYPE_AUTO_PROXY_URL) && cur.autoConfigUrl == PacUrl(port);
}

bool ApplySystemProxy(int port) {
  PreviousProxy cur;
  if (!QueryCurrent(cur)) return false;
  if ((cur.flags & PROXY_TYPE_AUTO_PROXY_URL) && cur.autoConfigUrl == PacUrl(port)) return true;

  // Never record our own PAC as "previous" (e.g. after a crash or port change).
  if (!IsOurPacUrl(cur.autoConfigUrl) || !(cur.flags & PROXY_TYPE_AUTO_PROXY_URL)) {
    SavePrevious(cur);
    Log("proxy: saved previous settings (flags=0x%lx)", cur.flags);
  }
  // Manual proxy (if any) moves into the PAC; keep auto-detect as the user had it.
  DWORD flags = PROXY_TYPE_DIRECT | PROXY_TYPE_AUTO_PROXY_URL | (cur.flags & PROXY_TYPE_AUTO_DETECT);
  return SetSettings(flags, cur.server, cur.bypass, PacUrl(port));
}

bool RestoreSystemProxy() {
  PreviousProxy cur;
  if (!QueryCurrent(cur) || !IsOurPacUrl(cur.autoConfigUrl)) return true;  // someone else owns it now
  PreviousProxy prev;
  if (!LoadPrevious(prev)) prev = PreviousProxy{};
  bool ok = SetSettings(prev.flags, prev.server, prev.bypass, prev.autoConfigUrl);
  if (ok) Log("proxy: restored previous settings");
  return ok;
}

}  // namespace pf
