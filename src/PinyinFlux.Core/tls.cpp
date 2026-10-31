#include "tls.h"

#include "log.h"

namespace pf {

namespace {
CredHandle g_cred{};
bool g_hasCred = false;
constexpr size_t kMaxBuffered = 64 * 1024;
}  // namespace

bool TlsInitServerCredentials(PCCERT_CONTEXT cert) {
  TlsFreeServerCredentials();
  SCH_CREDENTIALS creds{};
  creds.dwVersion = SCH_CREDENTIALS_VERSION;
  creds.cCreds = 1;
  creds.paCred = &cert;
  creds.dwFlags = SCH_USE_STRONG_CRYPTO;
  TimeStamp expiry;
  SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
                                                 nullptr, &creds, nullptr, nullptr, &g_cred, &expiry);
  if (st != SEC_E_OK) {
    Log("tls: AcquireCredentialsHandle failed 0x%08lx", st);
    return false;
  }
  g_hasCred = true;
  return true;
}

void TlsFreeServerCredentials() {
  if (g_hasCred) FreeCredentialsHandle(&g_cred);
  g_hasCred = false;
}

TlsServerStream::~TlsServerStream() {
  if (hasContext_) DeleteSecurityContext(&context_);
}

bool TlsServerStream::RecvRaw() {
  if (encrypted_.size() > kMaxBuffered) return false;
  char buf[16384];
  int n = recv(socket_, buf, sizeof(buf), 0);
  if (n <= 0) return false;
  encrypted_.append(buf, n);
  return true;
}

bool TlsServerStream::SendRaw(const void* data, size_t length) {
  const char* p = static_cast<const char*>(data);
  while (length > 0) {
    int n = send(socket_, p, static_cast<int>(length), 0);
    if (n <= 0) return false;
    p += n;
    length -= n;
  }
  return true;
}

bool TlsServerStream::Handshake() {
  if (!g_hasCred) return false;
  constexpr ULONG kFlags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                           ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
  bool needData = true;
  while (true) {
    if (needData && !RecvRaw()) return false;
    needData = false;

    SecBuffer in[2] = {{static_cast<ULONG>(encrypted_.size()), SECBUFFER_TOKEN, encrypted_.data()},
                       {0, SECBUFFER_EMPTY, nullptr}};
    SecBuffer out[1] = {{0, SECBUFFER_TOKEN, nullptr}};
    SecBufferDesc inDesc{SECBUFFER_VERSION, 2, in};
    SecBufferDesc outDesc{SECBUFFER_VERSION, 1, out};
    ULONG attrs = 0;
    SECURITY_STATUS st = AcceptSecurityContext(&g_cred, hasContext_ ? &context_ : nullptr, &inDesc, kFlags, 0,
                                               &context_, &outDesc, &attrs, nullptr);
    if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) hasContext_ = true;

    if (out[0].cbBuffer && out[0].pvBuffer) {
      bool sent = SendRaw(out[0].pvBuffer, out[0].cbBuffer);
      FreeContextBuffer(out[0].pvBuffer);
      if (!sent) return false;
    }

    if (st == SEC_E_INCOMPLETE_MESSAGE) {
      needData = true;
      continue;
    }
    if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) {
      Log("tls: handshake failed 0x%08lx", st);
      return false;
    }

    if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0) {
      encrypted_.erase(0, encrypted_.size() - in[1].cbBuffer);
    } else {
      encrypted_.clear();
    }

    if (st == SEC_E_OK) {
      return QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_) == SEC_E_OK;
    }
    needData = encrypted_.empty();
  }
}

int TlsServerStream::Recv(char* buffer, int length) {
  while (plainPos_ >= plain_.size()) {
    plain_.clear();
    plainPos_ = 0;
    if (encrypted_.empty() && !RecvRaw()) return 0;

    SecBuffer bufs[4] = {{static_cast<ULONG>(encrypted_.size()), SECBUFFER_DATA, encrypted_.data()},
                         {0, SECBUFFER_EMPTY, nullptr},
                         {0, SECBUFFER_EMPTY, nullptr},
                         {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
    SECURITY_STATUS st = DecryptMessage(&context_, &desc, 0, nullptr);
    if (st == SEC_E_INCOMPLETE_MESSAGE) {
      if (!RecvRaw()) return 0;
      continue;
    }
    if (st == SEC_I_CONTEXT_EXPIRED) return 0;  // close_notify
    if (st != SEC_E_OK && st != SEC_I_RENEGOTIATE) {
      Log("tls: decrypt failed 0x%08lx", st);
      return -1;
    }

    std::string extra;
    for (const SecBuffer& b : bufs) {
      if (b.BufferType == SECBUFFER_DATA && b.cbBuffer) plain_.append(static_cast<char*>(b.pvBuffer), b.cbBuffer);
      if (b.BufferType == SECBUFFER_EXTRA && b.cbBuffer) extra.assign(static_cast<char*>(b.pvBuffer), b.cbBuffer);
    }
    encrypted_ = std::move(extra);
    // SEC_I_RENEGOTIATE carries TLS 1.3 post-handshake messages (e.g. key
    // update). Short IME request bursts don't need them; end the connection
    // cleanly and let WinHTTP reconnect.
    if (st == SEC_I_RENEGOTIATE && plain_.empty()) return 0;
  }
  int n = static_cast<int>(std::min<size_t>(length, plain_.size() - plainPos_));
  memcpy(buffer, plain_.data() + plainPos_, n);
  plainPos_ += n;
  return n;
}

bool TlsServerStream::Send(const char* data, size_t length) {
  std::string packet;
  while (length > 0) {
    size_t chunk = std::min<size_t>(length, sizes_.cbMaximumMessage);
    packet.resize(sizes_.cbHeader + chunk + sizes_.cbTrailer);
    memcpy(packet.data() + sizes_.cbHeader, data, chunk);
    SecBuffer bufs[4] = {{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, packet.data()},
                         {static_cast<ULONG>(chunk), SECBUFFER_DATA, packet.data() + sizes_.cbHeader},
                         {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, packet.data() + sizes_.cbHeader + chunk},
                         {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
    if (EncryptMessage(&context_, 0, &desc, 0) != SEC_E_OK) return false;
    if (!SendRaw(packet.data(), bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer)) return false;
    data += chunk;
    length -= chunk;
  }
  return true;
}

void TlsServerStream::Shutdown() {
  if (!hasContext_) return;
  DWORD type = SCHANNEL_SHUTDOWN;
  SecBuffer token{sizeof(type), SECBUFFER_TOKEN, &type};
  SecBufferDesc tokenDesc{SECBUFFER_VERSION, 1, &token};
  if (ApplyControlToken(&context_, &tokenDesc) != SEC_E_OK) return;

  SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
  SecBufferDesc outDesc{SECBUFFER_VERSION, 1, &out};
  ULONG attrs = 0;
  constexpr ULONG kFlags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                           ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
  AcceptSecurityContext(&g_cred, &context_, nullptr, kFlags, 0, &context_, &outDesc, &attrs, nullptr);
  if (out.pvBuffer) {
    SendRaw(out.pvBuffer, out.cbBuffer);
    FreeContextBuffer(out.pvBuffer);
  }
}

}  // namespace pf
