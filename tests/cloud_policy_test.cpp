// SPDX-License-Identifier: MIT
#include "../src/PinyinFlux.Core/engine.h"
#include "../src/PinyinFlux.Core/providers/sogou.h"
#include <cstdio>
#include <thread>

using namespace pf;
struct FakeProvider final : Provider {
  int delay, calls = 0;
  bool success;
  std::vector<Candidate> answer;
  FakeProvider(int ms, bool ok, std::vector<Candidate> words) : delay(ms), success(ok), answer(std::move(words)) {}
  const char* Name() const override { return "fixture"; }
  bool Query(const std::string&, int, std::vector<Candidate>& out) override {
    ++calls; Sleep(delay); out = answer; return success;
  }
};
int main() {
  std::string payload(20, '\0'); payload[18] = 1;
  auto u16 = [&](uint16_t number) { payload += static_cast<char>(number); payload += static_cast<char>(number >> 8); };
  std::wstring word = L"绝区零";
  u16(6); payload.append(reinterpret_cast<const char*>(word.data()), 6);
  u16(6); u16(0x90); u16(0x108); u16(0xb4);
  u16(6); u16(3); u16(5); u16(9); payload += '\1';
  std::vector<Candidate> decoded;
  if (!DecodeSogouResponse("juequling", payload, decoded) || decoded.size() != 1 ||
      NativeReading("juequling", decoded[0]) != "jue qu ling") return 6;
  puts("PASS Sogou binary syllable IDs use authoritative input boundaries");
  payload.pop_back(); decoded.clear();
  if (DecodeSogouResponse("juequling", payload, decoded)) return 7;
  puts("PASS truncated Sogou record is rejected");
  Candidate valid{"绝区零", 9, true, "jue'qu'ling"};
  Candidate duplicate = valid;
  Candidate invalid{"绝区零", 9, true, "binary"};
  auto slow = std::make_shared<FakeProvider>(300, true, std::vector<Candidate>{valid});
  auto fast = std::make_shared<FakeProvider>(10, true, std::vector<Candidate>{valid, duplicate});
  Engine agreement({slow, fast}, 900, 4);
  auto start = GetTickCount64();
  auto result = agreement.QueryCandidates("juequling", true);
  auto elapsed = GetTickCount64() - start;
  if (result.size() != 1 || result[0].text != valid.text || elapsed < 280 || elapsed >= 450) return 1;
  puts("PASS native query waits for every source within its budget; duplicate removed");
  Candidate homophone{"绝曲零", 9, true, "jue'qu'ling"};
  auto preferred = std::make_shared<FakeProvider>(15, true, std::vector<Candidate>{homophone});
  auto backup = std::make_shared<FakeProvider>(1, true, std::vector<Candidate>{valid});
  Engine disagreement({preferred, backup}, 450, 4);
  result = disagreement.QueryCandidates("juequling", true);
  if (!result.empty()) return 8;
  puts("PASS full pinyin the sources disagree on offers no cloud word");
  auto unreadable = std::make_shared<FakeProvider>(1, true, std::vector<Candidate>{invalid});
  auto readable = std::make_shared<FakeProvider>(25, true, std::vector<Candidate>{valid});
  Engine quality({unreadable, readable}, 450, 4);
  result = quality.QueryCandidates("juequling", true);
  if (result.size() != 1 || NativeReading("juequling", result[0]).empty()) return 2;
  puts("PASS invalid preferred reading does not hide a usable backup");
  quality.QueryCandidates("juequling", true);
  if (readable->calls != 1) return 3;
  puts("PASS repeated native query reuses successful cache");
  auto broken = std::make_shared<FakeProvider>(1, false, std::vector<Candidate>{});
  Engine retry({broken}, 50, 4);
  retry.QueryCandidates("juequling", true); retry.QueryCandidates("juequling", true);
  if (broken->calls != 2) return 4;
  puts("PASS failed network response is retried instead of negatively cached");
  auto stalled = std::make_shared<FakeProvider>(600, true, std::vector<Candidate>{valid});
  Engine deadline({stalled}, 2000, 4);
  start = GetTickCount64();
  result = deadline.QueryCandidates("juequling", true);
  if (!result.empty() || GetTickCount64() - start > 570) return 5;
  puts("PASS native lookup deadline bounds a stalled provider");
  Sleep(650); // allow isolated worker fixtures to finish before process exit
  if (!NativeReading("jue", Candidate{"绝", 3, true, "jue"}).empty() ||
      !NativeReading("juequling", Candidate{"绝区0", 9, true, "jue'qu'ling"}).empty() ||
      !NativeReading("juequling", Candidate{"绝区=", 9, true, "jue'qu'ling"}).empty() ||
      NativeReading("lvxing", Candidate{"旅行", 6, true, "lü'xing"}) != "lv xing" ||
      NativeReading("yiling", Candidate{"一〇", 6, true, "yi'ling"}) != "yi ling") return 9;
  puts("PASS single characters and non-ideographs rejected; ü readings normalized");
  // Readings below are what Baidu returns for abbreviated input (covered = full reading length).
  Candidate genshin{"原神启动", 14, true, "yuan'shen'qi'dong"};
  struct Abbreviation { const char* input; Candidate candidate; const char* expected; ReadingMatch match; };
  const Abbreviation abbreviations[] = {
    {"yuanshenqd", genshin, "yuan shen qi dong", ReadingMatch::Abbreviated},
    {"ysqd", genshin, "yuan shen qi dong", ReadingMatch::Abbreviated},
    {"y's'q'd", genshin, "yuan shen qi dong", ReadingMatch::Abbreviated},
    {"yuanshenqidong", genshin, "yuan shen qi dong", ReadingMatch::Full},
    {"yyds", {"永远的神", 15, true, "yong'yuan'de'shen"}, "yong yuan de shen", ReadingMatch::Abbreviated},
    {"zhongguor", {"中国人", 11, true, "zhong'guo'ren"}, "zhong guo ren", ReadingMatch::Abbreviated},
    {"zhgr", {"中国人", 11, true, "zhong'guo'ren"}, "zhong guo ren", ReadingMatch::Abbreviated},
    {"zhonggr", {"中国人", 11, true, "zhong'guo'ren"}, "zhong guo ren", ReadingMatch::Abbreviated},
    {"xi'an", {"西安", 4, true, "xi'an"}, "xi an", ReadingMatch::Full},
    // A vowel-initial syllable needs a separator: the local engine reads "xian".
    {"xian", {"西安", 4, true, "xi'an"}, "", ReadingMatch::None},
    // English forced into pinyin (Baidu: de'e'pi'se'e'ka'mo'xing).
    {"deepseekmoxing", {"的恶评瑟尔卡模型", 14, true, "de'e'pi'se'e'ka'mo'xing"}, "", ReadingMatch::None},
    {"jintianqhh", {"今天去还好", 15, true, "jin'tian'qu'hai'hao"}, "jin tian qu hai hao", ReadingMatch::Abbreviated},
    // Covers only part of the input although Baidu reports covered >= input.
    {"jintianqhh", {"今天去回", 12, true, "jin'tian'qu'hui"}, "", ReadingMatch::None},
    {"ysqdx", genshin, "", ReadingMatch::None},
    // A typed separator must fall between syllables.
    {"yua'nshenqd", genshin, "", ReadingMatch::None},
    {"xian", {"先", 4, true, "xian"}, "", ReadingMatch::None},
    // Letters out of syllable order.
    {"sydq", genshin, "", ReadingMatch::None},
  };
  for (const auto& item : abbreviations) {
    if (NativeReading(item.input, item.candidate) != item.expected || MatchReading(item.input, item.candidate) != item.match) {
      printf("FAIL abbreviation %s -> %s\n", item.input, item.candidate.text.c_str()); return 16;
    }
  }
  puts("PASS abbreviated, initial-only and incomplete syllables align; partial coverage and forced English rejected");
  const std::vector<bool> both{true, true}, onlyFirst{true, false};
  // Sogou answers abbreviations without a full reading; borrow Baidu's.
  Candidate sogouGenshin{"原神启动", 10, true, ""};
  result = RankCandidates({{sogouGenshin}, {genshin}}, both, "yuanshenqd", true, 4);
  if (result.size() != 1 || NativeReading("yuanshenqd", result[0]) != "yuan shen qi dong") return 17;
  result = RankCandidates({{sogouGenshin}}, {true}, "yuanshenqd", true, 4);
  if (!result.empty()) return 18;
  puts("PASS reading-less abbreviated answer borrows a verified reading, else is dropped");
  // Observed answers. Abbreviations keep the preferred source's guess.
  Candidate yyds{"永远的神", 15, true, "yong'yuan'de'shen"}, generic{"永远都是", 16, true, "yong'yuan'dou'shi"};
  result = RankCandidates({{yyds, generic}, {Candidate{"永远都是", 4, true, ""}}}, both, "yyds", true, 1);
  if (result.size() != 1 || result[0].text != yyds.text) return 10;
  // Full pinyin: Baidu answers a homophone, Sogou the word the local dictionary has.
  Candidate wrong{"中果人", 11, true, "zhong'guo'ren"}, right{"中国人", 11, true, "zhong'guo'ren"};
  result = RankCandidates({{wrong}, {right}}, both, "zhongguoren", true, 1);
  if (!result.empty()) return 11;
  result = RankCandidates({{homophone, valid}, {Candidate{"决曲零", 9, true, "jue'qu'ling"}, valid}}, both, "juequling", true, 1);
  if (result.size() != 1 || result[0].text != valid.text) return 12;
  // A single usable answer counts once every other source has answered.
  result = RankCandidates({{valid}, {}}, both, "juequling", true, 1);
  if (result.size() != 1) return 13;
  result = RankCandidates({{valid}, {}}, onlyFirst, "juequling", true, 1);
  if (!result.empty()) return 19;
  Candidate mash{"但是家里发大水了咖啡机", 22, true, "dan'shi'jia'li'fa'da'shui'le'ka'fei'ji"};
  Candidate mashOther{"但是家里反对十六块腹肌", 11, true, ""};
  result = RankCandidates({{mash}, {mashOther}}, both, "dsjlfdslkfj", true, 1);
  if (!result.empty()) return 20;
  Candidate prc{"中华人民共和国", 21, true, "zhong'hua'ren'min'gong'he'guo"};
  result = RankCandidates({{prc}, {Candidate{"中华人民共和国", 7, true, ""}}}, both, "zhrmghg", true, 1);
  if (result.size() != 1 || result[0].text != prc.text) return 21;
  puts("PASS abbreviations keep the preferred guess; full pinyin needs agreement or an uncontested answer");
  puts("PASS long initials need a second source");
  // Up to three words: the best, then confirmed ones (two sources, each in its top two).
  Candidate nihao{"你好", 5, true, "ni'hao"}, nihao2{"你号", 5, true, "ni'hao"}, nihao3{"泥嚎", 5, true, "ni'hao"};
  result = RankCandidates({{nihao, nihao2, nihao3}, {nihao, nihao2, nihao3}}, both, "nihao", true, 3);
  if (result.size() != 2 || result[0].text != nihao.text || result[1].text != nihao2.text) return 22;
  Candidate yongyuan{"永远滴神", 15, true, "yong'yuan'di'shen"};
  result = RankCandidates({{yyds, generic, yongyuan}, {Candidate{"永远都是", 4, true, ""}, Candidate{"永远滴神", 4, true, ""}}},
                          both, "yyds", true, 3);
  if (result.size() != 2 || result[0].text != yyds.text || result[1].text != generic.text) return 23;
  puts("PASS confirmed words follow the best cloud word; lower homophones stay out");
  auto late = std::make_shared<FakeProvider>(600, true, std::vector<Candidate>{homophone, valid});
  auto early = std::make_shared<FakeProvider>(5, true, std::vector<Candidate>{valid});
  Engine backfill({late, early}, 2000, 4);
  result = backfill.QueryCandidates("juequling", true);
  if (!result.empty()) return 14;
  Sleep(300);
  result = backfill.QueryCandidates("juequling", true);
  if (late->calls != 1 || early->calls != 1 || result.size() != 1 || result[0].text != valid.text ||
      backfill.Stats().backfills != 1) return 15;
  puts("PASS answer missing the deadline is backfilled into the cache and then agreed on");
}
