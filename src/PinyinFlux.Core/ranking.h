#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"
#include "providers/provider.h"

namespace pf {

// Input kinds rank differently (docs/cloud-candidates.md): full pinyin needs
// agreement between sources, abbreviations trust the strongest source's guess.
enum RankMode { kFullPinyin = 0, kAbbreviated = 1, kRankModes };

// What the adaptive model has learned, for one ranking. Empty weights mean the
// configured priority alone.
struct RankPolicy {
  std::vector<double> weights[kRankModes];  // by provider index
  // Words after the best one need this fraction of its score.
  double threshold[kRankModes] = {0.5, 0.5};
};

struct Ranking {
  std::vector<Candidate> words;
  // ranks[w][p]: position of words[w] in provider p's usable answer, -1 if absent.
  std::vector<std::vector<int>> ranks;
  RankMode mode = kAbbreviated;
};

// A candidate for the whole spelling; a native one also needs a reading the
// input method can align with the spelling. Shared by ranking and waiting.
bool Usable(const std::string& pinyin, const Candidate& c, bool nativeOnly);

// Merges per-provider answers (index = configured priority; answered = the
// provider responded) into at most `limit` words. Native answers pass the
// fixed safety gates first, then are ordered by score; see ranking.cpp.
Ranking RankCandidates(const std::vector<std::vector<Candidate>>& results, const std::vector<bool>& answered,
                       const std::string& pinyin, bool nativeOnly, size_t limit, const RankPolicy& policy = {});

// Learns from which shown cloud word the user commits: provider weights per
// input kind (adaptive priority) and how close to the best word a further one
// must score to be shown (adaptive count). Keeps counts only, never text, in
// %LOCALAPPDATA%\PinyinFlux\ranking.ini.
class RankModel {
 public:
  // An empty path keeps the model in memory (tests, --dev, --test).
  explicit RankModel(std::wstring path = {});
  static std::wstring DefaultPath();

  RankPolicy Policy(const std::vector<std::string>& providers) const;
  // A native answer was sent to the input method; remembered briefly so a
  // later choice (which names the answer by AnswerHash only) can be traced
  // back to the sources that proposed each word.
  void Served(const std::vector<std::string>& providers, const Ranking& ranking);
  // shown: bit i = the placeholder displayed word i of the answer (it was no
  // local candidate); picked: index of the committed word, or -1 for another.
  // Returns false when the answer is unknown.
  bool Choose(DWORD answer, DWORD shown, int picked);
  // Writes the file if anything changed since the last call.
  void Save();
  // ,"ranking":{...} for the status endpoint.
  std::string Json() const;

 private:
  struct Counts {
    double hits = 0, seen = 0;
  };
  struct Answer {
    DWORD hash = 0;
    RankMode mode = kAbbreviated;
    std::vector<std::string> providers;
    std::vector<std::vector<int>> ranks;
  };
  // Pick rate of all `providers` together, and of one, shrunk towards the pooled rate.
  double PooledLocked(RankMode mode, const std::vector<std::string>& providers) const;
  double RateLocked(RankMode mode, const std::string& provider, double pooled) const;
  double ThresholdLocked(RankMode mode) const;

  std::wstring path_;
  mutable std::mutex mutex_;
  std::map<std::string, Counts> sources_[kRankModes];  // by provider name: survives reordering
  Counts extra_[kRankModes];                           // words shown after the first
  std::vector<Answer> recent_;                         // ring of served answers
  size_t next_ = 0;
  bool dirty_ = false;
};

}  // namespace pf
