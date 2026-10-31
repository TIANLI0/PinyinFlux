#include "cloud_query.h"
#include "json.h"
#include "common.h"
#include "providers/provider.h"
#include <algorithm>
#include <vector>

namespace pf {
std::string NormalizePinyin(std::string_view q) {
  if (q.size() > 200) return {};
  std::string out;
  for (char ch : q) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    if (ch >= 'a' && ch <= 'z') out += ch;
    else if (ch == '\'' || ch == ' ') {
      if (!out.empty() && out.back() != '\'') out += '\'';
    } else return {};
  }
  while (!out.empty() && out.back() == '\'') out.pop_back();
  return out.size() > 1 ? out : std::string();
}
std::string ExtractCloudPinyin(std::string_view query) {
  if (query.size() > 4096) return {};
  if (auto plain = NormalizePinyin(query); !plain.empty()) return plain;
  if (query.empty() || query.front() != '{' || query.back() != '}') return {};
  json::Value doc;
  if (json::Parse(query, doc) && doc.IsObject()) return NormalizePinyin(doc["q"].AsString());
  constexpr std::string_view marker = "_q_:_";
  size_t position = query.find(marker);
  if (position == std::string_view::npos || query.find(marker, position + marker.size()) != std::string_view::npos) return {};
  if (position == 0 || (query[position - 1] != ',' && query[position - 1] != '{')) return {};
  size_t start = position + marker.size(), end = query.find('_', start);
  if (end == std::string_view::npos || end + 1 >= query.size() || (query[end + 1] != ',' && query[end + 1] != '}')) return {};
  return NormalizePinyin(query.substr(start, end - start));
}
namespace {
// CJK ideographs (incl. extensions A-H and compatibility) and U+3007 〇.
bool Ideograph(char32_t c) {
  return c == 0x3007 || (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) ||
         (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x323af);
}
// Each syllable of the reading is typed in full or as a prefix ("qi", "q";
// "zhong", "zh", "zho"), covering the whole input. A typed separator falls
// between syllables, and a syllable starting with a vowel needs one ("xi'an"):
// unseparated, English such as "deepseek" would read de'e'pi'se'e'ka.
ReadingMatch Align(std::string_view input, const std::vector<std::string_view>& syllables) {
  // memo[i * (n + 1) + j]: 0 unknown, else ReadingMatch + 1. Input is at most 200 bytes.
  const size_t n = syllables.size();
  std::vector<char> memo((input.size() + 1) * (n + 1), 0);
  auto align = [&](auto&& self, size_t i, size_t j) -> ReadingMatch {
    if (j == n) return i == input.size() ? ReadingMatch::Full : ReadingMatch::None;
    if (i < input.size() && input[i] == '\'') {
      if (i == 0 || j == 0) return ReadingMatch::None;
      ++i;
    }
    const std::string_view syllable = syllables[j];
    bool separated = i > 0 && input[i - 1] == '\'';
    if (j > 0 && !separated && std::string_view("aoe").find(syllable.front()) != std::string_view::npos)
      return ReadingMatch::None;
    char& seen = memo[i * (n + 1) + j];
    if (seen) return static_cast<ReadingMatch>(seen - 1);
    ReadingMatch best = ReadingMatch::None;
    for (size_t k = std::min(syllable.size(), input.size() - i); k && best != ReadingMatch::Full; --k) {
      if (input.substr(i, k) != syllable.substr(0, k)) continue;
      ReadingMatch rest = self(self, i + k, j + 1);
      if (rest == ReadingMatch::None) continue;
      ReadingMatch here = k == syllable.size() && rest == ReadingMatch::Full ? ReadingMatch::Full : ReadingMatch::Abbreviated;
      if (here == ReadingMatch::Full || best == ReadingMatch::None) best = here;
    }
    seen = static_cast<char>(static_cast<int>(best) + 1);
    return best;
  };
  return align(align, 0, 0);
}
std::string Reading(std::string_view pinyin, const Candidate& candidate, ReadingMatch& match) {
  match = ReadingMatch::None;
  // Providers may spell ü; the keyboard input always uses v.
  std::string spelled = candidate.reading;
  for (std::string_view umlaut : {"\xc3\xbc", "u:"})
    for (size_t at; (at = spelled.find(umlaut)) != std::string::npos;) spelled.replace(at, umlaut.size(), "v");
  std::string reading = NormalizePinyin(spelled), input = NormalizePinyin(pinyin);
  if (!candidate.fullMatch || reading.empty() || input.empty()) return {};
  std::vector<std::string_view> syllables;
  for (size_t start = 0; start <= reading.size();) {
    size_t end = std::min(reading.find('\'', start), reading.size());
    syllables.push_back(std::string_view(reading).substr(start, end - start));
    start = end + 1;
  }
  // Native conversion does not check the reading against the input, so this
  // is the only guard: full, abbreviated (yuanshenqd, yyds) and incomplete
  // syllables align, while a word covering only part of the input does not.
  ReadingMatch aligned = Align(input, syllables);
  if (aligned == ReadingMatch::None) return {};
  const auto text = Utf8ToWide(candidate.text);
  size_t characters = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char32_t c = text[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
      c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
    // Mixed scripts and symbols cannot map one syllable per character.
    if (!Ideograph(c)) return {};
    ++characters;
  }
  // Every single character is already in the local dictionary; a cloud copy
  // would only displace a local candidate.
  if (characters < 2 || characters != syllables.size()) return {};
  match = aligned;
  std::replace(reading.begin(), reading.end(), '\'', ' ');
  return reading;
}
}  // namespace
std::string NativeReading(std::string_view pinyin, const Candidate& candidate) {
  ReadingMatch match;
  return Reading(pinyin, candidate, match);
}
ReadingMatch MatchReading(std::string_view pinyin, const Candidate& candidate) {
  ReadingMatch match;
  Reading(pinyin, candidate, match);
  return match;
}
}
