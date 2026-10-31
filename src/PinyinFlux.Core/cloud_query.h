#pragma once
#include <string>
#include <string_view>
namespace pf {
struct Candidate;
std::string NormalizePinyin(std::string_view query);
// Accept naked pinyin and Microsoft's CHS request envelope. Only return the
// spelling; application names, identifiers and other metadata stay local.
std::string ExtractCloudPinyin(std::string_view query);
// Empty means the provider answer cannot safely enter native conversion:
// partial coverage, mismatched reading, non-ideographs, a single character, or
// a character count different from the syllable count.
std::string NativeReading(std::string_view pinyin, const Candidate& candidate);
// Full: every syllable typed completely. Abbreviated: some typed as a prefix
// (yyds, yuanshenqd). None: NativeReading is empty.
enum class ReadingMatch { None, Full, Abbreviated };
ReadingMatch MatchReading(std::string_view pinyin, const Candidate& candidate);
}
