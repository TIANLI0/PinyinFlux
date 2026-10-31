#pragma once

#include "common.h"

#include <wincrypt.h>
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <subauth.h>
#include <security.h>
#include <schannel.h>

#include <string>

namespace pf {

// Process-wide Schannel server credentials for the interception certificate.
bool TlsInitServerCredentials(PCCERT_CONTEXT cert);
void TlsFreeServerCredentials();

// Server side of a TLS connection over an already-connected socket.
class TlsServerStream {
 public:
  explicit TlsServerStream(SOCKET socket) : socket_(socket) {}
  ~TlsServerStream();
  TlsServerStream(const TlsServerStream&) = delete;
  TlsServerStream& operator=(const TlsServerStream&) = delete;

  bool Handshake();
  // Returns decrypted bytes; 0 on orderly close, -1 on error.
  int Recv(char* buffer, int length);
  bool Send(const char* data, size_t length);
  bool Send(const std::string& data) { return Send(data.data(), data.size()); }
  void Shutdown();

 private:
  bool RecvRaw();
  bool SendRaw(const void* data, size_t length);

  SOCKET socket_;
  CtxtHandle context_{};
  bool hasContext_ = false;
  SecPkgContext_StreamSizes sizes_{};
  std::string encrypted_;  // received, not yet decrypted
  std::string plain_;      // decrypted, not yet returned
  size_t plainPos_ = 0;
};

}  // namespace pf
