#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "common.h"

namespace pf {

class Engine;

struct ProxyStats {
  std::atomic<uint64_t> imeSessions{0};     // TLS-terminated ChsIME connections
  std::atomic<uint64_t> tunnels{0};         // blind tunnels for other clients
  std::atomic<uint64_t> cloudRequests{0};   // qsonhs.aspx requests answered
};

// Loopback HTTP proxy. Only Bing hosts are accepted (that's all the PAC routes
// here); anything else is refused so this never acts as an open proxy.
class ProxyServer {
 public:
  ProxyServer() = default;
  ~ProxyServer();

  bool Start(int port);
  void Stop();

  void SetEngine(std::shared_ptr<Engine> engine) { engine_.store(std::move(engine)); }
  // Extra JSON fields (without braces) appended to /status.
  void SetStatusExtra(std::function<std::string()> fn) { statusExtra_ = std::move(fn); }

  const ProxyStats& Stats() const { return stats_; }
  int Port() const { return port_; }

 private:
  static DWORD WINAPI AcceptThread(void* self);
  static DWORD WINAPI ConnectionThread(void* context);

  void HandleConnection(SOCKET client);
  void ServeIntercepted(SOCKET client, const std::string& host);
  void ServeLocal(SOCKET client, const std::string& method, const std::string& target);
  std::string HandleCloudRequest(const std::string& query, std::string& contentType, bool nativeFormat = true);
  std::string StatusJson();

  SOCKET listener_ = INVALID_SOCKET;
  HANDLE acceptThread_ = nullptr;
  int port_ = 0;
  std::atomic<int> active_{0};
  std::atomic<std::shared_ptr<Engine>> engine_;
  std::function<std::string()> statusExtra_;
  ProxyStats stats_;
};

}  // namespace pf
