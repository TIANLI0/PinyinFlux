#pragma once

#include <string>

namespace pf {

// Points the user's WinINET proxy configuration at our PAC script, remembering
// what was configured before so it can be chained in the PAC and restored.
bool ApplySystemProxy(int port);
bool RestoreSystemProxy();
bool IsSystemProxyApplied(int port);

// The PAC served at /pinyinflux.pac: Bing hosts go through us (with DIRECT
// fallback when we're down); everything else follows the user's previous setup.
std::string BuildPacScript(int port);

std::wstring PacUrl(int port);

}  // namespace pf
