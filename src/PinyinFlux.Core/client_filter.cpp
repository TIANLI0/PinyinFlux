#include "client_filter.h"

#include <iphlpapi.h>
#include <tlhelp32.h>

#include <mutex>

namespace pf {

DWORD LoopbackClientPid(SOCKET accepted) {
  sockaddr_in peer{}, self{};
  int len = sizeof(peer);
  if (getpeername(accepted, reinterpret_cast<sockaddr*>(&peer), &len) != 0 || peer.sin_family != AF_INET) return 0;
  len = sizeof(self);
  if (getsockname(accepted, reinterpret_cast<sockaddr*>(&self), &len) != 0) return 0;

  std::vector<BYTE> buf;
  DWORD size = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    DWORD rc = GetExtendedTcpTable(buf.empty() ? nullptr : buf.data(), &size, FALSE, AF_INET,
                                   TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    if (rc == NO_ERROR) break;
    if (rc != ERROR_INSUFFICIENT_BUFFER) return 0;
    buf.resize(size + 4096);
    size = static_cast<DWORD>(buf.size());
  }
  if (buf.empty()) return 0;

  // The client's row has the peer's endpoint as its local side and ours as remote.
  auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const MIB_TCPROW_OWNER_PID& row = table->table[i];
    if (row.dwLocalAddr == peer.sin_addr.s_addr && static_cast<u_short>(row.dwLocalPort) == peer.sin_port &&
        row.dwRemoteAddr == self.sin_addr.s_addr && static_cast<u_short>(row.dwRemotePort) == self.sin_port) {
      return row.dwOwningPid;
    }
  }
  return 0;
}

bool IsImeProcess(DWORD pid) {
  if (pid == 0) return false;
  // ChsIME.exe is long-lived; remember the last positive match.
  static std::mutex mutex;
  static DWORD knownImePid = 0;
  {
    std::lock_guard lock(mutex);
    if (pid == knownImePid) return true;
  }

  // A process snapshot gives image names without opening the process.
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry{sizeof(entry)};
  bool match = false;
  for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
    if (entry.th32ProcessID == pid) {
      match = _wcsicmp(entry.szExeFile, L"ChsIME.exe") == 0;
      break;
    }
  }
  CloseHandle(snap);
  if (match) {
    std::lock_guard lock(mutex);
    knownImePid = pid;
  }
  return match;
}

}  // namespace pf
