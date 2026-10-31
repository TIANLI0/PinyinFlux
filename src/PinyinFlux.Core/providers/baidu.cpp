// Baidu IME cloud pinyin.
//
//   GET https://olimenew.baidu.com/py?inputtype=py&resultcoding=utf-8&input=<pinyin>
//   {"status":"T","errno":"0","result":[[["我想吃饭",13,{"pinyin":"wo'xiang'chi'fan",...}], ...]]}
//
// The number is the length of input consumed by the candidate.

#include "baidu.h"

#include "../common.h"
#include "../config.h"
#include "../http_client.h"
#include "../json.h"
#include "../log.h"

namespace pf {

BaiduProvider::BaiduProvider(const Config& config) : baseUrl_(config.baiduUrl), http_(std::make_unique<HttpClient>()) {}
BaiduProvider::~BaiduProvider() = default;

bool BaiduProvider::Query(const std::string& rawPinyin, int timeoutMs, std::vector<Candidate>& out) {
  std::string pinyin = StripSeparators(rawPinyin);
  if (pinyin.empty() || pinyin.size() > 200) return false;
  HttpResponse resp = http_->Send(L"GET", baseUrl_ + UrlEncode(pinyin), {}, L"Accept: application/json", timeoutMs);
  if (resp.status != 200) return false;

  json::Value doc;
  if (!json::Parse(resp.body, doc)) {
    Log("baidu: invalid json");
    return false;
  }
  if (doc["status"].AsString() != "T" || doc["errno"].AsString() != "0") {
    Log("baidu: api error %s/%s", doc["status"].AsString().c_str(), doc["errno"].AsString().c_str());
    return false;
  }

  const int inputLen = static_cast<int>(pinyin.size());
  for (const json::Value& item : doc["result"][0].array) {
    std::string text = item[0].AsString();
    if (text.empty() || IsAsciiOnly(text)) continue;  // pure ASCII echoes add nothing
    Candidate c;
    c.text = std::move(text);
    c.covered = item[1].AsInt();
    c.fullMatch = c.covered >= inputLen;
    c.reading = item[2]["pinyin"].AsString();
    out.push_back(std::move(c));
  }
  return !out.empty();
}

}  // namespace pf
