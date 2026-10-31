#pragma once

#include "common.h"

namespace pf {

// Identifies the process on the other end of a loopback TCP connection, so only
// Microsoft Pinyin's own connections get TLS-terminated. Other clients that
// reach us through the PAC (browsers visiting bing.com) get a blind tunnel and
// are never decrypted.
DWORD LoopbackClientPid(SOCKET accepted);
bool IsImeProcess(DWORD pid);

}  // namespace pf
