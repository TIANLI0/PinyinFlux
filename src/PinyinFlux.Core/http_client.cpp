#include "http_client.h"

#include <winhttp.h>

#include "log.h"

namespace pf {

namespace {
struct InternetHandle {
  HINTERNET h = nullptr;
  ~InternetHandle() {
    if (h) WinHttpCloseHandle(h);
  }
};

std::string QueryHeaderString(HINTERNET request, DWORD info) {
  wchar_t buf[256];
  DWORD size = sizeof(buf);
  if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size, WINHTTP_NO_HEADER_INDEX))
    return {};
  return WideToUtf8(std::wstring_view(buf, size / sizeof(wchar_t)));
}
}  // namespace

HttpClient::HttpClient() {
  session_ = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", WINHTTP_ACCESS_TYPE_NO_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session_) {
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
}

HttpClient::~HttpClient() {
  if (session_) WinHttpCloseHandle(session_);
}

HttpResponse HttpClient::Send(const std::wstring& method, const std::string& url, const std::string& body,
                              const std::wstring& extraHeaders, int timeoutMs, const std::string& hostOverride,
                              size_t maxBody) {
  HttpResponse resp;
  if (!session_) return resp;

  std::wstring wurl = Utf8ToWide(url);
  URL_COMPONENTS uc{sizeof(uc)};
  wchar_t host[256], path[2048];
  uc.lpszHostName = host;
  uc.dwHostNameLength = ARRAYSIZE(host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = ARRAYSIZE(path);
  wchar_t extra[2048];
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = ARRAYSIZE(extra);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    Log("http: bad url %s", url.c_str());
    return resp;
  }
  std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
  bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

  InternetHandle connect{WinHttpConnect(session_, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0)};
  if (!connect.h) return resp;

  InternetHandle request{WinHttpOpenRequest(connect.h, method.c_str(), object.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            secure ? WINHTTP_FLAG_SECURE : 0)};
  if (!request.h) return resp;
  WinHttpSetTimeouts(request.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

  std::wstring headers = extraHeaders;
  if (!hostOverride.empty()) {
    if (!headers.empty() && headers.back() != L'\n') headers += L"\r\n";
    headers += L"Host: " + Utf8ToWide(hostOverride) + L"\r\n";
  }

  if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                          headers.empty() ? 0 : static_cast<DWORD>(-1L),
                          body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                          static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    Log("http: %s %s failed: %lu", WideToUtf8(method).c_str(), url.c_str(), GetLastError());
    return resp;
  }

  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  resp.contentType = QueryHeaderString(request.h, WINHTTP_QUERY_CONTENT_TYPE);
  resp.contentEncoding = QueryHeaderString(request.h, WINHTTP_QUERY_CONTENT_ENCODING);

  char buf[8192];
  DWORD read = 0;
  for (;;) {
    if (!WinHttpReadData(request.h, buf, sizeof(buf), &read)) return resp;
    if (read == 0) break;
    resp.body.append(buf, read);
    if (resp.body.size() > maxBody) return resp;  // status stays 0: treat as failure
  }
  resp.status = static_cast<int>(status);
  return resp;
}

}  // namespace pf
