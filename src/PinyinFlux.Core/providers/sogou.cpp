// Sogou mobile IME cloud protocol (as used by the Android client).
//
// Request (POST, application/octet-stream):
//   total length (1 byte) | token "\0\5\0\0\0\0\1" | pinyin length (1 byte) | pinyin | XOR checksum
// Response: 20-byte header with the record count as uint16 LE at offset 18, then per record:
//   u16 wordBytes | UTF-16LE word | u16 pinyinBytes | syllable IDs | u16 indexBytes | u16[] cumulative
//   input positions (last one = input consumed) | 1 flag byte
//
// Protocol notes from the MIT-licensed rime-cloud project (github.com/xiaobili/rime-cloud).

#include "sogou.h"

#include "../common.h"
#include "../config.h"
#include "../http_client.h"
#include "../log.h"

namespace pf {

namespace {

std::string BuildPayload(const std::string& pinyin) {
  static constexpr char kToken[] = {0, 5, 0, 0, 0, 0, 1};
  std::string data;
  data += static_cast<char>(sizeof(kToken) + pinyin.size() + 3);
  data.append(kToken, sizeof(kToken));
  data += static_cast<char>(pinyin.size());
  data += pinyin;
  unsigned char x = 0;
  for (unsigned char c : data) x ^= c;
  data += static_cast<char>(x);
  return data;
}

bool ReadU16(const std::string& b, size_t pos, uint16_t& v) {
  if (pos + 2 > b.size()) return false;
  v = static_cast<uint16_t>(static_cast<unsigned char>(b[pos]) | (static_cast<unsigned char>(b[pos + 1]) << 8));
  return true;
}

}  // namespace

SogouProvider::SogouProvider(const Config& config) : url_(config.sogouUrl), http_(std::make_unique<HttpClient>()) {
  // The hostname is DNS-filtered on some networks; optionally connect to a known IP.
  if (!config.sogouIp.empty()) {
    size_t schemeEnd = url_.find("://");
    if (schemeEnd != std::string::npos) {
      size_t hostStart = schemeEnd + 3;
      size_t hostEnd = url_.find_first_of(":/", hostStart);
      if (hostEnd == std::string::npos) hostEnd = url_.size();
      hostOverride_ = url_.substr(hostStart, hostEnd - hostStart);
      url_.replace(hostStart, hostEnd - hostStart, config.sogouIp);
    }
  }
}

SogouProvider::~SogouProvider() = default;

bool SogouProvider::Query(const std::string& rawPinyin, int timeoutMs, std::vector<Candidate>& out) {
  std::string pinyin = StripSeparators(rawPinyin);
  if (pinyin.empty() || pinyin.size() > 200) return false;

  HttpResponse resp = http_->Send(L"POST", url_, BuildPayload(pinyin),
                                  L"Content-Type: application/octet-stream\r\nAccept: */*", timeoutMs, hostOverride_);
  if (resp.status != 200) return false;
  return DecodeSogouResponse(pinyin, resp.body, out);
}

bool DecodeSogouResponse(const std::string& pinyin, const std::string& b, std::vector<Candidate>& out) {
  uint16_t count = 0;
  if (b.size() < 22 || !ReadU16(b, 18, count) || count == 0 || count > 32) {
    Log("sogou: bad response (%zu bytes)", b.size());
    return false;
  }

  const int inputLen = static_cast<int>(pinyin.size());
  size_t pos = 20;
  for (uint16_t i = 0; i < count; ++i) {
    uint16_t wordBytes, pinyinBytes, indexBytes;
    if (!ReadU16(b, pos, wordBytes) || wordBytes == 0 || wordBytes % 2 || pos + 2 + wordBytes > b.size()) break;
    pos += 2;
    std::wstring word(wordBytes / 2, 0);
    memcpy(word.data(), b.data() + pos, wordBytes);
    pos += wordBytes;
    if (!ReadU16(b, pos, pinyinBytes)) break;
    pos += 2;
    if (pos + pinyinBytes > b.size()) break;
    pos += pinyinBytes;
    if (!ReadU16(b, pos, indexBytes)) break;
    pos += 2;
    if (indexBytes % 2 || pos + indexBytes >= b.size()) break;
    uint16_t covered = 0;
    if (indexBytes >= 2) ReadU16(b, pos + indexBytes - 2, covered);
    // The pinyin payload contains numeric syllable IDs, not UTF-16 text.
    // Its cumulative input positions give exact boundaries for full pinyin.
    // Do not invent a full reading for abbreviated/incomplete syllables.
    std::string reading;
    size_t previous = 0;
    if (indexBytes / 2 == word.size()) {
      for (size_t offset = 0; offset < indexBytes; offset += 2) {
        uint16_t end = 0; ReadU16(b, pos + offset, end);
        if (end <= previous || end > pinyin.size() || end - previous > 6) { reading.clear(); break; }
        std::string part = pinyin.substr(previous, end - previous);
        if (part.size() == 1 && part != "a" && part != "o" && part != "e") { reading.clear(); break; }
        if (!reading.empty()) reading += '\'';
        reading += part;
        previous = end;
      }
    }
    pos += indexBytes + 1;  // trailing flag byte

    std::wstring clean;
    for (wchar_t ch : word) {
      if (ch >= 0x20) clean += ch;
    }
    std::string text = WideToUtf8(clean);
    if (text.empty() || IsAsciiOnly(text)) continue;
    Candidate c;
    c.text = std::move(text);
    c.covered = covered;
    c.fullMatch = covered >= inputLen;
    c.reading = std::move(reading);
    out.push_back(std::move(c));
  }
  return !out.empty();
}

}  // namespace pf
