#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pf {

struct Config;

struct Candidate {
  std::string text;  // UTF-8
  // How many characters of the provider's input this candidate consumes.
  // Only candidates covering the whole input can replace Bing's cloud slot.
  int covered = 0;
  bool fullMatch = false;
  std::string reading; // complete provider reading, with apostrophe/space syllable boundaries
};

// A cloud lexicon source. Implementations must be safe to call concurrently
// from multiple threads and must honor `timeoutMs`.
class Provider {
 public:
  virtual ~Provider() = default;
  virtual const char* Name() const = 0;
  // `pinyin` is lowercase ASCII letters, possibly with ' syllable separators.
  virtual bool Query(const std::string& pinyin, int timeoutMs, std::vector<Candidate>& out) = 0;
};

// Returns nullptr for unknown names. Register new providers here.
std::unique_ptr<Provider> CreateProvider(std::string_view name, const Config& config);
std::vector<std::string> KnownProviders();

// Shared helpers for provider implementations.
bool IsAsciiOnly(std::string_view s);
std::string StripSeparators(std::string_view pinyin);

}  // namespace pf
