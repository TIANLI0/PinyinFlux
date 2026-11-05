#include "engine.h"

#include <algorithm>

#include "common.h"
#include "config.h"
#include "log.h"

namespace pf {

namespace {

constexpr size_t kCacheCapacity = 512;
constexpr ULONGLONG kCacheTtlMs = 30 * 60 * 1000;
// Some provider failed or missed the deadline: retry the preferred source soon.
constexpr ULONGLONG kPartialTtlMs = 60 * 1000;
constexpr ULONGLONG kNegativeTtlMs = 2000;
constexpr ULONGLONG kBackupGraceMs = 40;
constexpr int kNativeBudgetMs = 450;
// Microsoft Pinyin removes cloud words it already lists and shows the rest
// from slot 2. How many of these are offered adapts (see RankCandidates).
constexpr size_t kNativeWords = 3;

// Shared between the waiting request thread and the worker callbacks, which
// may outlive the wait when a provider is slower than the timeout.
struct Fanout {
  Fanout(size_t n, std::shared_ptr<ResultCache> resultCache, std::shared_ptr<EngineStats> engineStats)
      : results(n), done(n, false), ok(n, false), cache(std::move(resultCache)), stats(std::move(engineStats)) {
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  ~Fanout() { CloseHandle(event); }

  std::mutex mutex;
  std::vector<std::vector<Candidate>> results;
  std::vector<bool> done;
  std::vector<bool> ok;  // answered successfully
  bool failed = false;
  size_t pending = 0;
  // Set when the request returned before every provider finished; the last
  // worker then caches the merged answer for the next lookup.
  bool backfill = false;
  HANDLE event;

  std::shared_ptr<ResultCache> cache;
  std::shared_ptr<EngineStats> stats;
  std::string key, pinyin;
  bool nativeOnly = false;
  size_t limit = 0;
  RankPolicy policy;
};

ULONGLONG TtlFor(const std::vector<Candidate>& words, bool complete) {
  if (words.empty()) return kNegativeTtlMs;
  return complete ? kCacheTtlMs : kPartialTtlMs;
}

struct Job {
  std::shared_ptr<Fanout> fanout;
  std::shared_ptr<Provider> provider;
  size_t index;
  int timeoutMs;
};

void CALLBACK RunJob(PTP_CALLBACK_INSTANCE, void* context) {
  std::unique_ptr<Job> job(static_cast<Job*>(context));
  Fanout& fanout = *job->fanout;
  std::vector<Candidate> found;
  ULONGLONG start = GetTickCount64();
  bool ok = job->provider->Query(fanout.pinyin, job->timeoutMs, found);
  Log("%s: %s -> %zu result(s) in %llums", job->provider->Name(), fanout.pinyin.c_str(), found.size(),
      GetTickCount64() - start);

  Answers answers;
  std::vector<Candidate> merged;
  bool complete = false, publish = false;
  {
    std::lock_guard lock(fanout.mutex);
    if (!ok) fanout.failed = true;
    if (ok) fanout.results[job->index] = std::move(found);
    fanout.ok[job->index] = ok;
    fanout.done[job->index] = true;
    --fanout.pending;
    if (fanout.pending == 0 && fanout.backfill) {
      complete = !fanout.failed;
      answers = {fanout.results, fanout.ok};
      merged = RankCandidates(answers.results, answers.answered, fanout.pinyin, fanout.nativeOnly, fanout.limit,
                              fanout.policy).words;
      // Never replace a usable partial answer with a failure.
      publish = !merged.empty() || complete;
    }
    SetEvent(fanout.event);
  }
  if (publish) {
    fanout.cache->Put(fanout.key, answers, TtlFor(merged, complete));
    ++fanout.stats->backfills;
    Log("engine: %s backfilled %zu word(s)%s", fanout.pinyin.c_str(), merged.size(), complete ? "" : " (partial)");
  }
}

}  // namespace

bool ResultCache::Get(const std::string& key, Answers& out) {
  std::lock_guard lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  if (GetTickCount64() > it->second.expiresAt) {
    lru_.erase(it->second.lru);
    entries_.erase(it);
    return false;
  }
  lru_.splice(lru_.begin(), lru_, it->second.lru);
  out = it->second.answers;
  return true;
}

void ResultCache::Put(const std::string& key, const Answers& answers, ULONGLONG ttlMs) {
  std::lock_guard lock(mutex_);
  ULONGLONG expires = GetTickCount64() + ttlMs;
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    it->second.answers = answers;
    it->second.expiresAt = expires;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return;
  }
  lru_.push_front(key);
  entries_.emplace(key, Entry{answers, expires, lru_.begin()});
  if (entries_.size() > kCacheCapacity) {
    entries_.erase(lru_.back());
    lru_.pop_back();
  }
}

Engine::Engine(const Config& config, std::shared_ptr<RankModel> model)
    : timeoutMs_(config.timeoutMs), maxCandidates_(static_cast<size_t>(config.maxCandidates)),
      model_(model ? std::move(model) : std::make_shared<RankModel>()) {
  for (const auto& name : config.providers) {
    if (auto p = CreateProvider(name, config)) {
      names_.emplace_back(p->Name());
      providers_.push_back(std::move(p));
    } else {
      Log("engine: unknown provider '%s' ignored", name.c_str());
    }
  }
}

Engine::~Engine() = default;
Engine::Engine(std::vector<std::shared_ptr<Provider>> providers, int timeoutMs, size_t maxCandidates,
               std::shared_ptr<RankModel> model)
    : providers_(std::move(providers)), timeoutMs_(timeoutMs), maxCandidates_(maxCandidates),
      model_(model ? std::move(model) : std::make_shared<RankModel>()) {
  for (const auto& p : providers_) names_.emplace_back(p->Name());
}

std::vector<std::string> Engine::ProviderNames() const { return names_; }

std::string Engine::LastQuery() const {
  std::lock_guard lock(lastMutex_);
  return lastQuery_;
}

std::vector<std::string> Engine::Query(const std::string& pinyin) {
  std::vector<std::string> words;
  for (const auto& candidate : QueryCandidates(pinyin)) words.push_back(candidate.text);
  return words;
}

std::vector<Candidate> Engine::QueryCandidates(const std::string& pinyin, bool nativeOnly) {
  ++stats_->queries;
  {
    std::lock_guard lock(lastMutex_);
    lastQuery_ = pinyin;
  }

  std::vector<Candidate> words;
  const size_t limit = nativeOnly ? kNativeWords : maxCandidates_;
  const RankPolicy policy = model_->Policy(names_);
  auto rank = [&](const Answers& answers) {
    Ranking ranking = RankCandidates(answers.results, answers.answered, pinyin, nativeOnly, limit, policy);
    // Remembered so that the user's choice among these words is learned from.
    if (nativeOnly) model_->Served(names_, ranking);
    return std::move(ranking.words);
  };
  const std::string cacheKey = (nativeOnly ? "native:" : "query:") + pinyin;
  if (Answers cached; cache_->Get(cacheKey, cached)) {
    ++stats_->cacheHits;
    words = rank(cached);
    if (!words.empty()) ++stats_->answered;
    return words;
  }
  if (providers_.empty()) return words;

  ULONGLONG start = GetTickCount64();
  // Late network answers must not reshuffle a candidate list indefinitely.
  const int budget = nativeOnly ? std::min(timeoutMs_, kNativeBudgetMs) : timeoutMs_;
  auto fanout = std::make_shared<Fanout>(providers_.size(), cache_, stats_);
  fanout->key = cacheKey;
  fanout->pinyin = pinyin;
  fanout->nativeOnly = nativeOnly;
  fanout->limit = limit;
  fanout->policy = policy;
  fanout->pending = providers_.size();
  for (size_t i = 0; i < providers_.size(); ++i) {
    auto* job = new Job{fanout, providers_[i], i, budget};
    if (!TrySubmitThreadpoolCallback(RunJob, job, nullptr)) {
      delete job;
      std::lock_guard lock(fanout->mutex);
      fanout->failed = true;
      fanout->done[i] = true;
      if (--fanout->pending == 0) SetEvent(fanout->event);
    }
  }
  auto usable = [&](const Candidate& c) { return Usable(pinyin, c, nativeOnly); };
  ULONGLONG firstAnswer = 0;
  for (;;) {
    bool found = false, preferred = false, done = false;
    {
      std::lock_guard lock(fanout->mutex);
      bool higherDone = true;
      for (size_t i = 0; i < fanout->results.size(); ++i) {
        if (std::any_of(fanout->results[i].begin(), fanout->results[i].end(), usable)) {
          found = true; preferred = higherDone; break;
        }
        higherDone &= fanout->done[i];
      }
      done = fanout->pending == 0;
    }
    const ULONGLONG now = GetTickCount64();
    if (found && !firstAnswer) firstAnswer = now;
    // Native answers need every source to judge agreement. The placeholder is
    // only added once a word arrives, so waiting never moves local candidates.
    bool settled = !nativeOnly && (preferred || (firstAnswer && now - firstAnswer >= kBackupGraceMs));
    if (done || settled || now - start >= static_cast<ULONGLONG>(budget)) break;
    ULONGLONG remaining = budget - (now - start);
    if (firstAnswer && !nativeOnly) remaining = std::min<ULONGLONG>(remaining, kBackupGraceMs - (now - firstAnswer));
    WaitForSingleObject(fanout->event, static_cast<DWORD>(remaining));
  }

  bool complete;
  {
    std::lock_guard lock(fanout->mutex);
    complete = fanout->pending == 0 && !fanout->failed;
    Answers answers{fanout->results, fanout->ok};
    words = rank(answers);
    // Misses are cached only when every provider actually answered, so a
    // transient timeout does not suppress the word for the negative-cache
    // window. Written under the fanout lock: a straggler's backfill (RunJob)
    // must not be overwritten by this older partial answer.
    if (!words.empty() || complete) cache_->Put(cacheKey, answers, TtlFor(words, complete));
    fanout->backfill = fanout->pending != 0;
  }

  ULONGLONG elapsed = GetTickCount64() - start;
  stats_->totalLatencyMs += elapsed;
  if (!words.empty()) ++stats_->answered;
  Log("engine: %s -> %zu word(s) in %llums%s", pinyin.c_str(), words.size(), elapsed, complete ? "" : " (partial)");
  return words;
}

}  // namespace pf
