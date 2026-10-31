#include "proxy_server.h"

#include "bing_format.h"
#include "client_filter.h"
#include "cloud_query.h"
#include "config.h"
#include "engine.h"
#include "http_client.h"
#include "json.h"
#include "log.h"
#include "sysproxy.h"
#include "tls.h"

namespace pf {

namespace {

constexpr int kMaxConnections = 128;
constexpr size_t kMaxHeadBytes = 32 * 1024;
constexpr size_t kMaxBodyBytes = 1024 * 1024;
constexpr DWORD kIdleTimeoutMs = 120 * 1000;

bool IsBingHost(const std::string& host) { return host == "cn.bing.com" || host == "www.bing.com"; }

struct HttpHead {
  std::string method, target, version;
  std::vector<std::pair<std::string, std::string>> headers;

  std::string Header(std::string_view name) const {
    for (const auto& [k, v] : headers) {
      if (IEquals(k, name)) return v;
    }
    return {};
  }
};

bool ParseHead(std::string_view text, HttpHead& head) {
  size_t lineEnd = text.find("\r\n");
  std::string_view line = text.substr(0, lineEnd);
  size_t a = line.find(' ');
  size_t b = a == std::string_view::npos ? a : line.find(' ', a + 1);
  if (b == std::string_view::npos) return false;
  head.method = std::string(line.substr(0, a));
  head.target = std::string(line.substr(a + 1, b - a - 1));
  head.version = std::string(line.substr(b + 1));
  size_t pos = lineEnd == std::string_view::npos ? text.size() : lineEnd + 2;
  while (pos < text.size()) {
    size_t end = text.find("\r\n", pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view h = text.substr(pos, end - pos);
    size_t colon = h.find(':');
    if (colon != std::string_view::npos) {
      head.headers.emplace_back(std::string(Trim(h.substr(0, colon))), std::string(Trim(h.substr(colon + 1))));
    }
    pos = end + 2;
  }
  return true;
}

// Reads one request head (through "\r\n\r\n") using `read`, keeping any bytes
// beyond it in `buffer` for the body / next request.
template <typename ReadFn>
bool ReadHead(ReadFn&& read, std::string& buffer, HttpHead& head) {
  size_t end;
  while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
    if (buffer.size() > kMaxHeadBytes) return false;
    char chunk[4096];
    int n = read(chunk, static_cast<int>(sizeof(chunk)));
    if (n <= 0) return false;
    buffer.append(chunk, n);
  }
  bool ok = ParseHead(std::string_view(buffer).substr(0, end), head);
  buffer.erase(0, end + 4);
  return ok;
}

template <typename ReadFn>
bool ReadBody(ReadFn&& read, std::string& buffer, size_t length, std::string& body) {
  if (length > kMaxBodyBytes) return false;
  while (buffer.size() < length) {
    char chunk[4096];
    int n = read(chunk, static_cast<int>(sizeof(chunk)));
    if (n <= 0) return false;
    buffer.append(chunk, n);
  }
  body = buffer.substr(0, length);
  buffer.erase(0, length);
  return true;
}

bool SendAll(SOCKET s, const char* data, size_t length) {
  while (length > 0) {
    int n = send(s, data, static_cast<int>(length), 0);
    if (n <= 0) return false;
    data += n;
    length -= n;
  }
  return true;
}

bool SendAll(SOCKET s, const std::string& data) { return SendAll(s, data.data(), data.size()); }

std::string BuildResponse(int status, const char* reason, const std::string& contentType, const std::string& body,
                          bool keepAlive, const std::string& extraHeaders = {}) {
  std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
  if (!contentType.empty()) r += "Content-Type: " + contentType + "\r\n";
  r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  r += extraHeaders;
  r += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
  r += body;
  return r;
}

SOCKET ConnectTo(const std::string& host, const std::string& port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) return INVALID_SOCKET;
  SOCKET s = INVALID_SOCKET;
  for (addrinfo* ai = result; ai; ai = ai->ai_next) {
    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET) continue;
    if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
    closesocket(s);
    s = INVALID_SOCKET;
  }
  freeaddrinfo(result);
  return s;
}

// Copies bytes both ways until either side closes or the link goes idle.
void Relay(SOCKET a, SOCKET b) {
  char buf[16384];
  while (true) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(a, &fds);
    FD_SET(b, &fds);
    timeval tv{static_cast<long>(kIdleTimeoutMs / 1000), 0};
    if (select(0, &fds, nullptr, nullptr, &tv) <= 0) return;
    for (SOCKET from : {a, b}) {
      if (!FD_ISSET(from, &fds)) continue;
      int n = recv(from, buf, sizeof(buf), 0);
      if (n <= 0 || !SendAll(from == a ? b : a, buf, n)) return;
    }
  }
}

struct ConnectionContext {
  ProxyServer* server;
  SOCKET socket;
};

}  // namespace

ProxyServer::~ProxyServer() { Stop(); }

bool ProxyServer::Start(int port) {
  listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener_ == INVALID_SOCKET) return false;
  BOOL exclusive = TRUE;
  setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener_, SOMAXCONN) != 0) {
    Log("proxy: cannot listen on 127.0.0.1:%d (%d)", port, WSAGetLastError());
    closesocket(listener_);
    listener_ = INVALID_SOCKET;
    return false;
  }
  port_ = port;
  acceptThread_ = CreateThread(nullptr, 64 * 1024, AcceptThread, this, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  Log("proxy: listening on 127.0.0.1:%d", port);
  return acceptThread_ != nullptr;
}

void ProxyServer::Stop() {
  if (listener_ != INVALID_SOCKET) {
    closesocket(listener_);
    listener_ = INVALID_SOCKET;
  }
  if (acceptThread_) {
    WaitForSingleObject(acceptThread_, 2000);
    CloseHandle(acceptThread_);
    acceptThread_ = nullptr;
  }
}

DWORD WINAPI ProxyServer::AcceptThread(void* self) {
  auto* server = static_cast<ProxyServer*>(self);
  while (true) {
    SOCKET client = accept(server->listener_, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      if (server->listener_ == INVALID_SOCKET) return 0;
      continue;
    }
    if (server->active_.load() >= kMaxConnections) {
      closesocket(client);
      continue;
    }
    ++server->active_;
    auto* ctx = new ConnectionContext{server, client};
    HANDLE t = CreateThread(nullptr, 256 * 1024, ConnectionThread, ctx, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (t) {
      CloseHandle(t);
    } else {
      --server->active_;
      closesocket(client);
      delete ctx;
    }
  }
}

DWORD WINAPI ProxyServer::ConnectionThread(void* context) {
  std::unique_ptr<ConnectionContext> ctx(static_cast<ConnectionContext*>(context));
  DWORD timeout = kIdleTimeoutMs;
  setsockopt(ctx->socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
  ctx->server->HandleConnection(ctx->socket);
  closesocket(ctx->socket);
  --ctx->server->active_;
  return 0;
}

void ProxyServer::HandleConnection(SOCKET client) {
  std::string buffer;
  HttpHead head;
  auto read = [client](char* p, int n) { return recv(client, p, n, 0); };
  if (!ReadHead(read, buffer, head)) return;

  if (head.method == "CONNECT") {
    if (CurrentConfig()->nativeInjection) {
      SendAll(client, BuildResponse(403, "Forbidden", {}, {}, false));
      return;
    }
    std::string target = ToLowerAscii(head.target);
    size_t colon = target.rfind(':');
    std::string host = target.substr(0, colon);
    std::string port = colon == std::string::npos ? "443" : target.substr(colon + 1);
    if (!IsBingHost(host)) {
      SendAll(client, BuildResponse(403, "Forbidden", {}, {}, false));
      return;
    }

    DWORD pid = LoopbackClientPid(client);
    bool intercept = port == "443" && (!CurrentConfig()->imeOnly || IsImeProcess(pid));
    if (intercept) {
      SendAll(client, "HTTP/1.1 200 Connection Established\r\n\r\n");
      ++stats_.imeSessions;
      Log("proxy: intercepting %s for pid %lu", host.c_str(), pid);
      ServeIntercepted(client, host);
      return;
    }

    SOCKET upstream = ConnectTo(host, port);
    if (upstream == INVALID_SOCKET) {
      SendAll(client, BuildResponse(502, "Bad Gateway", {}, {}, false));
      return;
    }
    SendAll(client, "HTTP/1.1 200 Connection Established\r\n\r\n");
    ++stats_.tunnels;
    if (!buffer.empty()) SendAll(upstream, buffer);
    Relay(client, upstream);
    closesocket(upstream);
    return;
  }

  if (!head.target.empty() && head.target[0] == '/') {
    ServeLocal(client, head.method, head.target);
    return;
  }

  // Plain-HTTP proxy request ("GET http://cn.bing.com/... HTTP/1.1").
  if (!CurrentConfig()->nativeInjection && IStartsWith(head.target, "http://")) {
    std::string rest = head.target.substr(7);
    size_t slash = rest.find('/');
    std::string authority = ToLowerAscii(rest.substr(0, slash));
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = authority.find(':');
    std::string host = authority.substr(0, colon);
    if (!IsBingHost(host)) {
      SendAll(client, BuildResponse(403, "Forbidden", {}, {}, false));
      return;
    }
    SOCKET upstream = ConnectTo(host, colon == std::string::npos ? "80" : authority.substr(colon + 1));
    if (upstream == INVALID_SOCKET) {
      SendAll(client, BuildResponse(502, "Bad Gateway", {}, {}, false));
      return;
    }
    std::string request = head.method + " " + path + " " + head.version + "\r\n";
    for (const auto& [k, v] : head.headers) {
      if (!IStartsWith(k, "Proxy-")) request += k + ": " + v + "\r\n";
    }
    request += "\r\n" + buffer;
    if (SendAll(upstream, request)) Relay(client, upstream);
    closesocket(upstream);
    return;
  }

  SendAll(client, BuildResponse(400, "Bad Request", {}, {}, false));
}

void ProxyServer::ServeIntercepted(SOCKET client, const std::string& host) {
  TlsServerStream tls(client);
  if (!tls.Handshake()) return;

  auto read = [&tls](char* p, int n) { return tls.Recv(p, n); };
  std::string buffer;
  while (true) {
    HttpHead head;
    if (!ReadHead(read, buffer, head)) break;
    std::string body;
    size_t length = static_cast<size_t>(_atoi64(head.Header("Content-Length").c_str()));
    if (length && !ReadBody(read, buffer, length, body)) break;
    bool keepAlive = !IEquals(head.Header("Connection"), "close");

    Log("mitm: %s %s", head.method.c_str(), head.target.c_str());
    std::string response;
    if (IStartsWith(head.target, "/qsonhs.aspx")) {
      size_t q = head.target.find('?');
      std::string contentType;
      std::string json = HandleCloudRequest(q == std::string::npos ? "" : head.target.substr(q + 1), contentType);
      response = BuildResponse(200, "OK", contentType, json, keepAlive, "Cache-Control: private, max-age=600\r\n");
    } else {
      // Anything else the IME asks of Bing is relayed unchanged.
      std::wstring headers;
      for (const auto& [k, v] : head.headers) {
        if (IEquals(k, "Host") || IEquals(k, "Connection") || IEquals(k, "Content-Length") ||
            IEquals(k, "Accept-Encoding") || IStartsWith(k, "Proxy-")) {
          continue;
        }
        headers += Utf8ToWide(k + ": " + v + "\r\n");
      }
      HttpClient http;
      HttpResponse r = http.Send(Utf8ToWide(head.method), "https://" + host + head.target, body, headers, 10000);
      if (r.status == 0) {
        response = BuildResponse(502, "Bad Gateway", {}, {}, false);
        keepAlive = false;
      } else {
        std::string extra = r.contentEncoding.empty() ? "" : "Content-Encoding: " + r.contentEncoding + "\r\n";
        response = BuildResponse(r.status, "OK", r.contentType, r.body, keepAlive, extra);
      }
    }
    if (!tls.Send(response) || !keepAlive) break;
  }
  tls.Shutdown();
}

std::string ProxyServer::HandleCloudRequest(const std::string& query, std::string& contentType, bool nativeFormat) {
  contentType = "application/json; charset=utf-8";
  ++stats_.cloudRequests;
  std::string q = QueryParam(query, "q");
  std::string pinyin = ExtractCloudPinyin(q);
  Log("cloud: pinyin=\"%s\"", pinyin.c_str());
  std::vector<std::string> words;
  if (!pinyin.empty()) {
    if (auto engine = engine_.load()) {
      if (nativeFormat) return BuildNativeBingResponse(q, pinyin, engine->QueryCandidates(pinyin, true));
      words = engine->Query(pinyin);
    }
  }
  return BuildBingResponse(q, words);
}

void ProxyServer::ServeLocal(SOCKET client, const std::string& method, const std::string& target) {
  std::string path = target.substr(0, target.find('?'));
  if (method == "GET" && path == "/pinyinflux.pac") {
    // A fetch here means some WinHTTP/WinINET client is resolving its proxy through us.
    Log("pac: served to pid %lu", LoopbackClientPid(client));
    SendAll(client, BuildResponse(200, "OK", "application/x-ns-proxy-autoconfig", BuildPacScript(port_), false,
                                  "Cache-Control: no-cache\r\n"));
  } else if (method == "GET" && path == "/status") {
    SendAll(client, BuildResponse(200, "OK", "application/json; charset=utf-8", StatusJson(), false,
                                  "Cache-Control: no-store\r\n"));
  } else if (method == "GET" && (path == "/query" || path == "/qsonhs.aspx")) {
    // Debug helper: /query?q=nihao returns exactly what the IME would get.
    std::string contentType;
    std::string json = HandleCloudRequest(target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1),
                                          contentType, path == "/qsonhs.aspx");
    SendAll(client, BuildResponse(200, "OK", contentType, json, false));
  } else {
    SendAll(client, BuildResponse(404, "Not Found", "text/plain", "not found", false));
  }
}

std::string ProxyServer::StatusJson() {
  std::string s = "{\"version\":";
  json::AppendQuoted(s, kVersion);
  s += ",\"port\":" + std::to_string(port_);
  s += ",\"imeSessions\":" + std::to_string(stats_.imeSessions.load());
  s += ",\"tunnels\":" + std::to_string(stats_.tunnels.load());
  s += ",\"cloudRequests\":" + std::to_string(stats_.cloudRequests.load());
  if (auto engine = engine_.load()) {
    const EngineStats& es = engine->Stats();
    uint64_t misses = es.queries.load() - es.cacheHits.load();
    s += ",\"queries\":" + std::to_string(es.queries.load());
    s += ",\"cacheHits\":" + std::to_string(es.cacheHits.load());
    s += ",\"answered\":" + std::to_string(es.answered.load());
    s += ",\"backfills\":" + std::to_string(es.backfills.load());
    s += ",\"avgLatencyMs\":" + std::to_string(misses ? es.totalLatencyMs.load() / misses : 0);
    s += ",\"lastQuery\":";
    json::AppendQuoted(s, engine->LastQuery());
    s += ",\"providers\":[";
    auto names = engine->ProviderNames();
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) s += ',';
      json::AppendQuoted(s, names[i]);
    }
    s += ']';
  }
  if (statusExtra_) s += statusExtra_();
  s += '}';
  return s;
}

}  // namespace pf
