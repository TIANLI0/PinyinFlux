#include "ranking.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "../PinyinFlux.Inject/protocol.h"
#include "cloud_query.h"
#include "json.h"
#include "log.h"

namespace pf {

namespace {

// Before any choice, configured priority i weighs 1 / (1 + i/4).
double PriorWeight(size_t priority) { return 1.0 / (1.0 + 0.25 * static_cast<double>(priority)); }
// Agreement is the signal for full pinyin (a lone source may answer a
// homophone: zhongguoren -> 中果人) but favours generic phrases for
// abbreviations (yyds -> 永远都是), where it only breaks near ties.
constexpr double kAgreement[kRankModes] = {1.0, 0.2};

// Provider weight = prior * lift, lift = the source's pick rate / the pooled
// rate of all sources of that input kind, within [1/kMaxLift, kMaxLift]. Only
// the ratio matters: scores are compared with each other, and real pick rates
// are low (most commits take a local word), so an absolute scale would pin
// every source at its bound and erase what was learned. A source's rate
// starts from the pooled one as if kPriorSeen words had been shown; the pooled
// rate starts from kPriorRate the same way.
constexpr double kPriorRate = 0.2, kPriorSeen = 50, kMaxLift = 4;
// Further words stay at the base threshold while picked kExtraTarget of the
// times they are shown; picked more often, the threshold drops (more words),
// less often it rises (fewer).
constexpr double kExtraTarget = 0.08, kExtraPrior = 50;
constexpr double kBaseThreshold = 0.5, kMinThreshold = 0.25, kMaxThreshold = 0.95;
// Every choice fades older ones (half-life about 140 choices of that kind),
// which also drifts a threshold that stopped showing further words back
// towards the base, so they get shown and judged again.
constexpr double kDecay = 0.995;
// A later slot is looked at less often; its words are not judged as harshly.
constexpr double kExamined[] = {1.0, 0.7, 0.5};
constexpr size_t kRecentAnswers = 32;
const char* const kModeNames[kRankModes] = {"full", "abbr"};

}  // namespace

bool Usable(const std::string& pinyin, const Candidate& c, bool nativeOnly) {
  return c.fullMatch && (!nativeOnly || !NativeReading(pinyin, c).empty());
}

Ranking RankCandidates(const std::vector<std::vector<Candidate>>& results, const std::vector<bool>& answered,
                       const std::string& pinyin, bool nativeOnly, size_t limit, const RankPolicy& policy) {
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
  Ranking out;
  out.mode = full ? kFullPinyin : kAbbreviated;
  auto ranksOf = [&](const std::string& text) {
    std::vector<int> ranks(lists.size(), -1);
    for (size_t p = 0; p < lists.size(); ++p) {
      auto it = std::find_if(lists[p].begin(), lists[p].end(), [&](const Candidate& c) { return c.text == text; });
      if (it != lists[p].end()) ranks[p] = static_cast<int>(it - lists[p].begin());
    }
    return ranks;
  };
  auto add = [&](const Candidate& c) {
    if (out.words.size() < limit &&
        std::none_of(out.words.begin(), out.words.end(), [&](const Candidate& w) { return w.text == c.text; })) {
      out.words.push_back(c);
      out.ranks.push_back(ranksOf(c.text));
    }
  };
  if (!nativeOnly) {
    for (const auto& list : lists)
      for (const auto& c : list) add(c);
    return out;
  }

  std::vector<double> weight(lists.size());
  const auto& learned = policy.weights[out.mode];
  for (size_t p = 0; p < lists.size(); ++p) weight[p] = learned.size() == lists.size() ? learned[p] : PriorWeight(p);
  struct Scored { const Candidate* candidate; double score; };
  std::vector<Scored> scored;
  for (const auto& list : lists)
    for (const auto& c : list) {
      if (std::any_of(scored.begin(), scored.end(), [&](const Scored& s) { return s.candidate->text == c.text; })) continue;
      const std::vector<int> ranks = ranksOf(c.text);
      const auto sources = std::count_if(ranks.begin(), ranks.end(), [](int r) { return r >= 0; });
      if (full) {
        // Full pinyin: the local dictionary already has the common word. A
        // word must be confirmed: given by two sources, each ranking it among
        // its first two; further down are homophones (nihao -> 泥嚎).
        if (sources < 2 || std::any_of(ranks.begin(), ranks.end(), [](int r) { return r > 1; })) continue;
      } else {
        // Long initials are guesswork (dsjlfdslkfj -> 但是家里发大水了咖啡机):
        // a word of five or more characters needs a second source (zhrmghg ->
        // 中华人民共和国).
        const std::string reading = NativeReading(pinyin, c);
        size_t characters = static_cast<size_t>(std::count(reading.begin(), reading.end(), ' ')) + 1;
        if (characters >= 5 && lists.size() >= 2 && sources < 2) continue;
      }
      // Each source contributes its weight, decaying with the rank it gave.
      double best = 0, sum = 0;
      for (size_t p = 0; p < ranks.size(); ++p) {
        if (ranks[p] < 0) continue;
        double share = weight[p] / (1.0 + ranks[p]);
        best = std::max(best, share);
        sum += share;
      }
      scored.push_back({&c, best + kAgreement[out.mode] * (sum - best)});
    }
  if (full && scored.empty()) {
    // No agreement: the first answer of the only source with one counts once
    // every other source has answered (and found nothing usable).
    size_t answering = static_cast<size_t>(std::count_if(lists.begin(), lists.end(), [](const auto& list) { return !list.empty(); }));
    if (answering == 1 && std::all_of(answered.begin(), answered.end(), [](bool b) { return b; }))
      for (const auto& list : lists)
        if (!list.empty()) add(list.front());
    return out;
  }
  // Ties keep priority, then rank, order.
  std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
  for (const auto& s : scored)
    if (out.words.empty() || s.score >= policy.threshold[out.mode] * scored.front().score - 1e-9) add(*s.candidate);
  return out;
}

RankModel::RankModel(std::wstring path) : path_(std::move(path)) {
  if (path_.empty()) return;
  // mode extra hits seen | mode source <name> hits seen
  std::ifstream in(path_);
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string mode, kind, name;
    Counts counts;
    fields >> mode >> kind;
    if (kind == "source") fields >> name;
    fields >> counts.hits >> counts.seen;
    if (!fields || !std::isfinite(counts.hits) || !std::isfinite(counts.seen) || counts.hits < 0 || counts.seen < 0) continue;
    for (int m = 0; m < kRankModes; ++m) {
      if (mode != kModeNames[m]) continue;
      if (kind == "extra") extra_[m] = counts;
      else if (kind == "source" && !name.empty()) sources_[m][name] = counts;
    }
  }
}

std::wstring RankModel::DefaultPath() { return DataDir() + L"\\ranking.ini"; }

double RankModel::PooledLocked(RankMode mode, const std::vector<std::string>& providers) const {
  double hits = 0, seen = 0;
  for (const auto& name : providers)
    if (auto it = sources_[mode].find(name); it != sources_[mode].end()) {
      hits += it->second.hits;
      seen += it->second.seen;
    }
  return (hits + kPriorSeen * kPriorRate) / (seen + kPriorSeen);
}

double RankModel::RateLocked(RankMode mode, const std::string& provider, double pooled) const {
  auto it = sources_[mode].find(provider);
  const Counts counts = it != sources_[mode].end() ? it->second : Counts{};
  return (counts.hits + kPriorSeen * pooled) / (counts.seen + kPriorSeen);
}

double RankModel::ThresholdLocked(RankMode mode) const {
  const Counts& counts = extra_[mode];
  const double rate = (counts.hits + kExtraPrior * kExtraTarget) / (counts.seen + kExtraPrior);
  return std::clamp(kBaseThreshold * std::sqrt(kExtraTarget / rate), kMinThreshold, kMaxThreshold);
}

RankPolicy RankModel::Policy(const std::vector<std::string>& providers) const {
  RankPolicy policy;
  std::lock_guard lock(mutex_);
  for (int m = 0; m < kRankModes; ++m) {
    const auto mode = static_cast<RankMode>(m);
    const double pooled = PooledLocked(mode, providers);
    for (size_t p = 0; p < providers.size(); ++p) {
      const double lift = std::clamp(RateLocked(mode, providers[p], pooled) / pooled, 1 / kMaxLift, kMaxLift);
      policy.weights[m].push_back(PriorWeight(p) * lift);
    }
    policy.threshold[m] = ThresholdLocked(mode);
  }
  return policy;
}

void RankModel::Served(const std::vector<std::string>& providers, const Ranking& ranking) {
  if (ranking.words.empty()) return;
  std::vector<std::wstring> texts;
  for (const auto& word : ranking.words) texts.push_back(Utf8ToWide(word.text));
  Answer answer{native::AnswerHash(texts), ranking.mode, providers, ranking.ranks};
  std::lock_guard lock(mutex_);
  // The replayed query serves the same answer again: keep one entry.
  auto same = std::find_if(recent_.begin(), recent_.end(), [&](const Answer& a) { return a.hash == answer.hash; });
  if (same != recent_.end()) *same = std::move(answer);
  else if (recent_.size() < kRecentAnswers) recent_.push_back(std::move(answer));
  else { recent_[next_] = std::move(answer); next_ = (next_ + 1) % kRecentAnswers; }
}

bool RankModel::Choose(DWORD hash, DWORD shown, int picked) {
  std::lock_guard lock(mutex_);
  auto answer = std::find_if(recent_.begin(), recent_.end(), [&](const Answer& a) { return a.hash == hash; });
  if (answer == recent_.end() || !shown) return false;
  const size_t words = std::min<size_t>(answer->ranks.size(), 32);
  if (picked >= static_cast<int>(words) || (picked >= 0 && !(shown >> picked & 1))) return false;
  auto& sources = sources_[answer->mode];
  auto& extra = extra_[answer->mode];
  for (auto& [name, counts] : sources) { counts.hits *= kDecay; counts.seen *= kDecay; }
  extra.hits *= kDecay; extra.seen *= kDecay;
  size_t position = 0;
  for (size_t w = 0; w < words; ++w) {
    if (!(shown >> w & 1)) continue;
    const bool chosen = static_cast<int>(w) == picked;
    const double examined = kExamined[std::min(position, std::size(kExamined) - 1)];
    // Credit every source that proposed the word, more for a higher rank.
    for (size_t p = 0; p < answer->ranks[w].size() && p < answer->providers.size(); ++p) {
      const int rank = answer->ranks[w][p];
      if (rank < 0) continue;
      const double credit = 1.0 / (1.0 + rank);
      Counts& counts = sources[answer->providers[p]];
      counts.seen += credit * examined;
      if (chosen) counts.hits += credit;
    }
    ++position;
    // Judged by place in the answer, not on screen: the threshold admits
    // words after the best one, and that one is often a local word the
    // native filter hid (wangluo -> 网络 hidden, 王洛 shown first).
    if (w > 0) {
      extra.seen += 1;
      if (chosen) extra.hits += 1;
    }
  }
  dirty_ = true;
  return true;
}

void RankModel::Save() {
  std::string text = "# PinyinFlux adaptive cloud ranking: decayed choice counts, never what was typed\n";
  {
    std::lock_guard lock(mutex_);
    if (!dirty_ || path_.empty()) return;
    dirty_ = false;
    char line[160];
    for (int m = 0; m < kRankModes; ++m) {
      std::snprintf(line, sizeof(line), "%s extra %.4f %.4f\n", kModeNames[m], extra_[m].hits, extra_[m].seen);
      text += line;
      for (const auto& [name, counts] : sources_[m]) {
        std::snprintf(line, sizeof(line), "%s source %s %.4f %.4f\n", kModeNames[m], name.c_str(), counts.hits, counts.seen);
        text += line;
      }
    }
  }
  // Written to a sibling and swapped in, so a crash never leaves half a file.
  std::wstring temp = path_ + L".tmp";
  bool written;
  {
    std::ofstream out(temp, std::ios::trunc | std::ios::binary);
    out << text;
    out.close();
    written = !out.fail();
  }
  if (written && MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
  Log("ranking: could not save %s (%lu)", WideToUtf8(path_).c_str(), GetLastError());
  // Try again on the next save.
  std::lock_guard lock(mutex_);
  dirty_ = true;
}

std::string RankModel::Json() const {
  std::string s = ",\"ranking\":{";
  std::lock_guard lock(mutex_);
  char number[32];
  for (int m = 0; m < kRankModes; ++m) {
    if (m) s += ',';
    json::AppendQuoted(s, kModeNames[m]);
    std::snprintf(number, sizeof(number), "%.3f", ThresholdLocked(static_cast<RankMode>(m)));
    s += std::string(":{\"threshold\":") + number + ",\"sources\":{";
    std::vector<std::string> names;
    for (const auto& entry : sources_[m]) names.push_back(entry.first);
    const double pooled = PooledLocked(static_cast<RankMode>(m), names);
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) s += ',';
      json::AppendQuoted(s, names[i]);
      // The learned pick rate; the weight is the configured priority times its
      // ratio to the pooled rate.
      std::snprintf(number, sizeof(number), "%.3f", RateLocked(static_cast<RankMode>(m), names[i], pooled));
      s += std::string(":") + number;
    }
    s += "}}";
  }
  return s + "}";
}

}  // namespace pf
