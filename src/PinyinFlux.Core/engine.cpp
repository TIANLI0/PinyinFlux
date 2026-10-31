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
// from slot 2. Words after the best one must be confirmed (see RankCandidates).
constexpr size_t kNativeWords = 3;

bool Usable(const std::string& pinyin, const Candidate& c, bool nativeOnly) {
  return c.fullMatch && (!nativeOnly || !NativeReading(pinyin, c).empty());
}

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
      merged = RankCandidates(fanout.results, fanout.ok, fanout.pinyin, fanout.nativeOnly, fanout.limit);
      // Never replace a usable partial answer with a failure.
      publish = !merged.empty() || complete;
    }
    SetEvent(fanout.event);
  }
  if (publish) {
    fanout.cache->Put(fanout.key, merged, TtlFor(merged, complete));
    ++fanout.stats->backfills;
    Log("engine: %s backfilled %zu word(s)%s", fanout.pinyin.c_str(), merged.size(), complete ? "" : " (partial)");
  }
}

}  // namespace

std::vector<Candidate> RankCandidates(const std::vector<std::vector<Candidate>>& results, const std::vector<bool>& answered,
                                      const std::string& pinyin, bool nativeOnly, size_t limit) {
  // Sogou reports syllable IDs, so its abbreviated answers carry no full
  // reading. Borrow one another source verified for the same word.
  std::vector<const Candidate*> readable;
  if (nativeOnly) {
    for (const auto& list : results)
      for (const auto& c : list)
        if (Usable(pinyin, c, true)) readable.push_back(&c);
  }
  std::vector<std::vector<Candidate>> lists(results.size());
  bool full = false;
  for (size_t i = 0; i < results.size(); ++i) {
    for (Candidate c : results[i]) {
      if (nativeOnly && !Usable(pinyin, c, true)) {
        auto donor = std::find_if(readable.begin(), readable.end(), [&](const Candidate* r) { return r->text == c.text; });
        if (donor != readable.end()) c.reading = (*donor)->reading;
      }
      if (!Usable(pinyin, c, nativeOnly)) continue;
      auto& list = lists[i];
      if (std::any_of(list.begin(), list.end(), [&](const Candidate& item) { return item.text == c.text; })) continue;
      full |= nativeOnly && MatchReading(pinyin, c) == ReadingMatch::Full;
      list.push_back(std::move(c));
    }
  }
  auto sources = [&](const std::string& text) {
    return std::count_if(lists.begin(), lists.end(), [&](const auto& list) {
      return std::any_of(list.begin(), list.end(), [&](const Candidate& c) { return c.text == text; });
    });
  };
  // A confirmed word is answered by at least two sources, each ranking it among
  // its first two: further down the lists are homophones (nihao -> 泥嚎).
  auto confirmed = [&](const std::string& text) {
    size_t agreeing = 0;
    for (const auto& list : lists) {
      auto it = std::find_if(list.begin(), list.end(), [&](const Candidate& c) { return c.text == text; });
      if (it == list.end()) continue;
      if (it - list.begin() > 1) return false;
      ++agreeing;
    }
    return agreeing >= 2;
  };
  std::vector<Candidate> words;
  auto add = [&](const Candidate& c) {
    if (words.size() < limit && std::none_of(words.begin(), words.end(), [&](const Candidate& w) { return w.text == c.text; }))
      words.push_back(c);
  };
  if (!full) {
    // Abbreviations: the preferred source's guess is the most specific
    // (yyds -> 永远的神); agreement favours generic phrases (永远都是), so they
    // only follow it. Long initials are guesswork (dsjlfdslkfj ->
    // 但是家里发大水了咖啡机): a word of five or more characters needs a second
    // source (zhrmghg -> 中华人民共和国).
    for (const auto& list : lists)
      for (const auto& c : list) {
        if (!nativeOnly) { add(c); continue; }
        const std::string reading = NativeReading(pinyin, c);
        size_t characters = static_cast<size_t>(std::count(reading.begin(), reading.end(), ' ')) + 1;
        if (characters >= 5 && lists.size() >= 2 && sources(c.text) < 2) continue;
        if (words.empty() || confirmed(c.text)) add(c);
      }
    return words;
  }
  // Full pinyin: the local dictionary already has the common word, and a
  // source may answer with a homophone instead (zhongguoren -> 中果人). Offer
  // only confirmed words, or the first answer of the only source with one
  // once every other source has answered.
  for (const auto& list : lists)
    for (const auto& c : list)
      if (confirmed(c.text)) add(c);
  size_t answering = static_cast<size_t>(std::count_if(lists.begin(), lists.end(), [](const auto& list) { return !list.empty(); }));
  if (words.empty() && answering == 1 && std::all_of(answered.begin(), answered.end(), [](bool b) { return b; }))
    for (const auto& list : lists)
      if (!list.empty()) add(list.front());
  return words;
}

bool ResultCache::Get(const std::string& key, std::vector<Candidate>& out) {
  std::lock_guard lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  if (GetTickCount64() > it->second.expiresAt) {
    lru_.erase(it->second.lru);
    entries_.erase(it);
    return false;
  }
  lru_.splice(lru_.begin(), lru_, it->second.lru);
  out = it->second.words;
  return true;
}

void ResultCache::Put(const std::string& key, const std::vector<Candidate>& words, ULONGLONG ttlMs) {
  std::lock_guard lock(mutex_);
  ULONGLONG expires = GetTickCount64() + ttlMs;
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    it->second.words = words;
    it->second.expiresAt = expires;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return;
  }
  lru_.push_front(key);
  entries_.emplace(key, Entry{words, expires, lru_.begin()});
  if (entries_.size() > kCacheCapacity) {
    entries_.erase(lru_.back());
    lru_.pop_back();
  }
}

Engine::Engine(const Config& config)
    : timeoutMs_(config.timeoutMs), maxCandidates_(static_cast<size_t>(config.maxCandidates)) {
  for (const auto& name : config.providers) {
    if (auto p = CreateProvider(name, config)) {
      providers_.push_back(std::move(p));
    } else {
      Log("engine: unknown provider '%s' ignored", name.c_str());
    }
  }
}

Engine::~Engine() = default;
Engine::Engine(std::vector<std::shared_ptr<Provider>> providers, int timeoutMs, size_t maxCandidates)
    : providers_(std::move(providers)), timeoutMs_(timeoutMs), maxCandidates_(maxCandidates) {}

std::vector<std::string> Engine::ProviderNames() const {
  std::vector<std::string> names;
  for (const auto& p : providers_) names.emplace_back(p->Name());
  return names;
}

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
  const std::string cacheKey = (nativeOnly ? "native:" : "query:") + pinyin;
  if (cache_->Get(cacheKey, words)) {
    ++stats_->cacheHits;
    if (!words.empty()) ++stats_->answered;
    return words;
  }
  if (providers_.empty()) return words;

  ULONGLONG start = GetTickCount64();
  // Late network answers must not reshuffle a candidate list indefinitely.
  const int budget = nativeOnly ? std::min(timeoutMs_, kNativeBudgetMs) : timeoutMs_;
  const size_t limit = nativeOnly ? kNativeWords : maxCandidates_;
  auto fanout = std::make_shared<Fanout>(providers_.size(), cache_, stats_);
  fanout->key = cacheKey;
  fanout->pinyin = pinyin;
  fanout->nativeOnly = nativeOnly;
  fanout->limit = limit;
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
    words = RankCandidates(fanout->results, fanout->ok, pinyin, nativeOnly, limit);
    // Misses are cached only when every provider actually answered, so a
    // transient timeout does not suppress the word for the negative-cache
    // window. Written under the fanout lock: a straggler's backfill (RunJob)
    // must not be overwritten by this older partial answer.
    if (!words.empty() || complete) cache_->Put(cacheKey, words, TtlFor(words, complete));
    fanout->backfill = fanout->pending != 0;
  }

  ULONGLONG elapsed = GetTickCount64() - start;
  stats_->totalLatencyMs += elapsed;
  if (!words.empty()) ++stats_->answered;
  Log("engine: %s -> %zu word(s) in %llums%s", pinyin.c_str(), words.size(), elapsed, complete ? "" : " (partial)");
  return words;
}

}  // namespace pf
