#pragma once

#include <string>

#include "common.h"

namespace pf {

struct HttpResponse {
  int status = 0;  // 0 = transport failure
  std::string contentType;
  std::string contentEncoding;
  std::string body;
};

// Thin WinHTTP wrapper. One session per instance; WinHTTP pools keep-alive
// connections per session, so repeated vendor queries reuse TLS connections.
// Always connects directly (never through the system proxy), which also keeps
// our own relayed bing.com traffic from looping back into PinyinFlux.
class HttpClient {
 public:
  HttpClient();
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // `extraHeaders` is CRLF-separated. `hostOverride` sends a Host header while
  // connecting to the URL's literal host (used to pin a server IP). A body
  // larger than `maxBody` fails.
  HttpResponse Send(const std::wstring& method, const std::string& url, const std::string& body,
                    const std::wstring& extraHeaders, int timeoutMs, const std::string& hostOverride = {},
                    size_t maxBody = 4 * 1024 * 1024);

 private:
  void* session_ = nullptr;  // HINTERNET; winhttp.h is kept out of this header (it clashes with wininet.h)
};

}  // namespace pf
