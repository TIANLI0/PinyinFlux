#pragma once

#include <memory>
#include <string>
#include <vector>

namespace pf {

// Persisted in %LOCALAPPDATA%\PinyinFlux\config.ini; shared with the settings UI.
struct Config {
  int port = 18765;
  std::vector<std::string> providers = {"baidu", "sogou"};  // order = priority
  int timeoutMs = 900;
  int maxCandidates = 4;
  bool nativeInjection = true; // active native candidate pipeline; no PAC or TLS interception
  bool imeOnly = true;      // only terminate TLS for ChsIME.exe connections
  bool logToFile = false;
  bool reclaimProxy = true; // re-apply the PAC if another tool overwrites it
  std::string sogouUrl =
      "http://shouji.sogou.com/web_ime/mobile.php?durtot=0&h=000000000000000&r=store_mf_wandoujia&v=3.7";
  std::string sogouIp;      // optional: connect to this IP instead of resolving the host
  std::string baiduUrl = "https://olimenew.baidu.com/py?inputtype=py&resultcoding=utf-8&input=";
};

std::wstring ConfigPath();
std::shared_ptr<const Config> LoadConfig();
void SaveConfig(const Config& config);

// Current config snapshot; cheap to call from any thread.
std::shared_ptr<const Config> CurrentConfig();
void SetCurrentConfig(std::shared_ptr<const Config> config);

}  // namespace pf
