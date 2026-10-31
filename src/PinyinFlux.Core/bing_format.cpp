#include "bing_format.h"

#include "json.h"
#include "cloud_query.h"
#include "common.h"
#include <algorithm>

namespace pf {

std::string BuildBingResponse(const std::string& query, const std::vector<std::string>& words) {
  std::string out = "{\"AS\":{\"Query\":";
  json::AppendQuoted(out, query);
  if (words.empty()) {
    out += ",\"FullResults\":0}}";
    return out;
  }
  out += ",\"FullResults\":1,\"Results\":[{\"Type\":\"AS\",\"Suggests\":[";
  for (size_t i = 0; i < words.size(); ++i) {
    if (i) out += ',';
    out += "{\"Txt\":";
    json::AppendQuoted(out, words[i]);
    // The IME parses SC/KA suggestions; AS is the outer result group only.
    out += ",\"Type\":\"SC\",\"Sk\":";
    json::AppendQuoted(out, i == 0 ? std::string() : "AS" + std::to_string(i));
    if (i == 0) out += ",\"HCS\":0";
    out += '}';
  }
  out += "]}]}}";
  return out;
}

std::string BuildNativeBingResponse(const std::string& query, const std::string& pinyin, const std::vector<Candidate>& candidates) {
  std::string out = "{\"AS\":{\"Query\":";
  json::AppendQuoted(out, query);
  out += ",\"FullResults\":1,\"Results\":[{\"Type\":\"AS\",\"Suggests\":[";
  bool first = true;
  size_t index = 0;
  for (const auto& candidate : candidates) {
    std::string reading = NativeReading(pinyin, candidate);
    if (reading.empty()) continue;
    const size_t syllables = static_cast<size_t>(std::count(reading.begin(), reading.end(), ' ')) + 1;
    const std::string suffix = index ? std::to_string(index) : "";
    auto field = [&](const char* key, const std::string& value) {
      if (!first) out += ',';
      first = false;
      out += "{\"Type\":\"KA\",\"Txt\":";
      json::AppendQuoted(out, std::string(key) + suffix + "=" + value);
      out += '}';
    };
    // Native conversion records require five or more named fields. SC is
    // search-only and does not enter the cloud conversion candidate slot.
    field("Cand", candidate.text);
    field("FReadStr", reading);
    field("DReadStr", reading);
    field("DCand", candidate.text);
    field("Source", "1");
    field("WordCnt", std::to_string(syllables));
    field("Score", "1.0");
    ++index;
  }
  if (first) return BuildBingResponse(query, {});
  out += "]}]}}";
  return out;
}

}  // namespace pf
