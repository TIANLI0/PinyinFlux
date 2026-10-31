#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "cloud_query.h"
#include "providers/provider.h"

namespace pf {

struct Config;

struct EngineStats {
  std::atomic<uint64_t> queries{0};
  std::atomic<uint64_t> cacheHits{0};
  std::atomic<uint64_t> answered{0};  // queries that produced at least one candidate
  std::atomic<uint64_t> totalLatencyMs{0};
  std::atomic<uint64_t> backfills{0}; // late provider answers merged into the cache
};

// LRU result cache. Shared with in-flight provider workers so answers that
// arrive after the request deadline can still improve the next lookup.
class ResultCache {
 public:
  bool Get(const std::string& key, std::vector<Candidate>& out);
  void Put(const std::string& key, const std::vector<Candidate>& words, ULONGLONG ttlMs);

 private:
  struct Entry {
    std::vector<Candidate> words;
    ULONGLONG expiresAt;
    std::list<std::string>::iterator lru;
  };
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::list<std::string> lru_;
};

// Merges per-provider answers (index = configured priority; answered = the
// provider responded). Native abbreviations keep priority order; native full
// pinyin needs agreement between sources, see engine.cpp.
std::vector<Candidate> RankCandidates(const std::vector<std::vector<Candidate>>& results, const std::vector<bool>& answered,
                                      const std::string& pinyin, bool nativeOnly, size_t limit);

// Fans a pinyin query out to the configured providers in parallel and merges
// their full-match candidates (see RankCandidates).
class Engine {
 public:
  explicit Engine(const Config& config);
  Engine(std::vector<std::shared_ptr<Provider>> providers, int timeoutMs, size_t maxCandidates);
  ~Engine();

  std::vector<std::string> Query(const std::string& pinyin);
  std::vector<Candidate> QueryCandidates(const std::string& pinyin, bool nativeOnly = false);

  const EngineStats& Stats() const { return *stats_; }
  std::vector<std::string> ProviderNames() const;
  std::string LastQuery() const;

 private:
  std::vector<std::shared_ptr<Provider>> providers_;
  int timeoutMs_;
  size_t maxCandidates_;

  std::shared_ptr<ResultCache> cache_ = std::make_shared<ResultCache>();

  mutable std::mutex lastMutex_;
  std::string lastQuery_;

  std::shared_ptr<EngineStats> stats_ = std::make_shared<EngineStats>();
};

}  // namespace pf
