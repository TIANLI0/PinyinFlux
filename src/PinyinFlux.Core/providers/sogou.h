#pragma once

#include "provider.h"

namespace pf {

class HttpClient;
bool DecodeSogouResponse(const std::string& pinyin, const std::string& response, std::vector<Candidate>& out);

class SogouProvider final : public Provider {
 public:
  explicit SogouProvider(const Config& config);
  ~SogouProvider() override;
  const char* Name() const override { return "sogou"; }
  bool Query(const std::string& pinyin, int timeoutMs, std::vector<Candidate>& out) override;

 private:
  std::string url_;
  std::string hostOverride_;  // set when connecting to a pinned IP
  std::unique_ptr<HttpClient> http_;
};

}  // namespace pf
