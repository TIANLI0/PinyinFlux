#pragma once

#include "provider.h"

namespace pf {

class HttpClient;

class BaiduProvider final : public Provider {
 public:
  explicit BaiduProvider(const Config& config);
  ~BaiduProvider() override;
  const char* Name() const override { return "baidu"; }
  bool Query(const std::string& pinyin, int timeoutMs, std::vector<Candidate>& out) override;

 private:
  std::string baseUrl_;
  std::unique_ptr<HttpClient> http_;
};

}  // namespace pf
