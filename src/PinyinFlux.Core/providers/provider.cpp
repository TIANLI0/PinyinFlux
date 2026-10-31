#include "provider.h"

#include "baidu.h"
#include "sogou.h"

namespace pf {

std::unique_ptr<Provider> CreateProvider(std::string_view name, const Config& config) {
  if (name == "baidu") return std::make_unique<BaiduProvider>(config);
  if (name == "sogou") return std::make_unique<SogouProvider>(config);
  return nullptr;
}

std::vector<std::string> KnownProviders() { return {"baidu", "sogou"}; }

bool IsAsciiOnly(std::string_view s) {
  for (unsigned char c : s) {
    if (c >= 0x80) return false;
  }
  return true;
}

std::string StripSeparators(std::string_view pinyin) {
  std::string out;
  out.reserve(pinyin.size());
  for (char c : pinyin) {
    if (c != '\'') out += c;
  }
  return out;
}

}  // namespace pf
