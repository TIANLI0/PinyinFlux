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
#include "ranking.h"

namespace pf {

struct Config;

struct EngineStats {
  std::atomic<uint64_t> queries{0};
  std::atomic<uint64_t> cacheHits{0};
  std::atomic<uint64_t> answered{0};  // queries that produced at least one candidate
  std::atomic<uint64_t> totalLatencyMs{0};
  std::atomic<uint64_t> backfills{0}; // late provider answers merged into the cache
};

// Every provider's answer to one spelling (index = configured priority).
// Cached unranked, so what the ranking model learns applies at once.
struct Answers {
  std::vector<std::vector<Candidate>> results;
  std::vector<bool> answered;  // the provider responded
};

// LRU result cache. Shared with in-flight provider workers so answers that
// arrive after the request deadline can still improve the next lookup.
class ResultCache {
 public:
  bool Get(const std::string& key, Answers& out);
  void Put(const std::string& key, const Answers& answers, ULONGLONG ttlMs);

 private:
  struct Entry {
    Answers answers;
    ULONGLONG expiresAt;
    std::list<std::string>::iterator lru;
  };
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::list<std::string> lru_;
};

// Fans a pinyin query out to the configured providers in parallel and merges
// their full-match candidates (see RankCandidates). Native answers are ranked
// with what `model` learned; without one, the engine learns in memory.
class Engine {
 public:
  explicit Engine(const Config& config, std::shared_ptr<RankModel> model = nullptr);
  Engine(std::vector<std::shared_ptr<Provider>> providers, int timeoutMs, size_t maxCandidates,
         std::shared_ptr<RankModel> model = nullptr);
  ~Engine();

  std::vector<std::string> Query(const std::string& pinyin);
  std::vector<Candidate> QueryCandidates(const std::string& pinyin, bool nativeOnly = false);

  const EngineStats& Stats() const { return *stats_; }
  std::vector<std::string> ProviderNames() const;
  std::string LastQuery() const;

 private:
  std::vector<std::shared_ptr<Provider>> providers_;
  std::vector<std::string> names_;
  int timeoutMs_;
  size_t maxCandidates_;

  std::shared_ptr<ResultCache> cache_ = std::make_shared<ResultCache>();
  std::shared_ptr<RankModel> model_;

  mutable std::mutex lastMutex_;
  std::string lastQuery_;

  std::shared_ptr<EngineStats> stats_ = std::make_shared<EngineStats>();
};

}  // namespace pf
