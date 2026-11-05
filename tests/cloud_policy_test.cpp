// SPDX-License-Identifier: MIT
#include "../src/PinyinFlux.Core/engine.h"
#include "../src/PinyinFlux.Core/providers/sogou.h"
#include "../src/PinyinFlux.Inject/protocol.h"
#include <cmath>
#include <cstdio>
#include <thread>

using namespace pf;
struct FakeProvider final : Provider {
  int delay, calls = 0;
  bool success;
  std::vector<Candidate> answer;
  const char* name;
  FakeProvider(int ms, bool ok, std::vector<Candidate> words, const char* id = "fixture")
      : delay(ms), success(ok), answer(std::move(words)), name(id) {}
  const char* Name() const override { return name; }
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
  result = RankCandidates({{sogouGenshin}, {genshin}}, both, "yuanshenqd", true, 4).words;
  if (result.size() != 1 || NativeReading("yuanshenqd", result[0]) != "yuan shen qi dong") return 17;
  result = RankCandidates({{sogouGenshin}}, {true}, "yuanshenqd", true, 4).words;
  if (!result.empty()) return 18;
  puts("PASS reading-less abbreviated answer borrows a verified reading, else is dropped");
  // Observed answers. Abbreviations keep the preferred source's guess.
  Candidate yyds{"永远的神", 15, true, "yong'yuan'de'shen"}, generic{"永远都是", 16, true, "yong'yuan'dou'shi"};
  result = RankCandidates({{yyds, generic}, {Candidate{"永远都是", 4, true, ""}}}, both, "yyds", true, 1).words;
  if (result.size() != 1 || result[0].text != yyds.text) return 10;
  // Full pinyin: Baidu answers a homophone, Sogou the word the local dictionary has.
  Candidate wrong{"中果人", 11, true, "zhong'guo'ren"}, right{"中国人", 11, true, "zhong'guo'ren"};
  result = RankCandidates({{wrong}, {right}}, both, "zhongguoren", true, 1).words;
  if (!result.empty()) return 11;
  result = RankCandidates({{homophone, valid}, {Candidate{"决曲零", 9, true, "jue'qu'ling"}, valid}}, both, "juequling", true, 1).words;
  if (result.size() != 1 || result[0].text != valid.text) return 12;
  // A single usable answer counts once every other source has answered.
  result = RankCandidates({{valid}, {}}, both, "juequling", true, 1).words;
  if (result.size() != 1) return 13;
  result = RankCandidates({{valid}, {}}, onlyFirst, "juequling", true, 1).words;
  if (!result.empty()) return 19;
  Candidate mash{"但是家里发大水了咖啡机", 22, true, "dan'shi'jia'li'fa'da'shui'le'ka'fei'ji"};
  Candidate mashOther{"但是家里反对十六块腹肌", 11, true, ""};
  result = RankCandidates({{mash}, {mashOther}}, both, "dsjlfdslkfj", true, 1).words;
  if (!result.empty()) return 20;
  Candidate prc{"中华人民共和国", 21, true, "zhong'hua'ren'min'gong'he'guo"};
  result = RankCandidates({{prc}, {Candidate{"中华人民共和国", 7, true, ""}}}, both, "zhrmghg", true, 1).words;
  if (result.size() != 1 || result[0].text != prc.text) return 21;
  puts("PASS abbreviations keep the preferred guess; full pinyin needs agreement or an uncontested answer");
  puts("PASS long initials need a second source");
  // Up to three words in score order; full pinyin only confirmed ones (two sources, each in its top two).
  Candidate nihao{"你好", 5, true, "ni'hao"}, nihao2{"你号", 5, true, "ni'hao"}, nihao3{"泥嚎", 5, true, "ni'hao"};
  result = RankCandidates({{nihao, nihao2, nihao3}, {nihao, nihao2, nihao3}}, both, "nihao", true, 3).words;
  if (result.size() != 2 || result[0].text != nihao.text || result[1].text != nihao2.text) return 22;
  Candidate yongyuan{"永远滴神", 15, true, "yong'yuan'di'shen"};
  result = RankCandidates({{yyds, generic, yongyuan}, {Candidate{"永远都是", 4, true, ""}, Candidate{"永远滴神", 4, true, ""}}},
                          both, "yyds", true, 3).words;
  if (result.size() != 2 || result[0].text != yyds.text || result[1].text != generic.text) return 23;
  puts("PASS further words need half the best score; lower homophones stay out");
  // Adaptive count and priority. Default scores: 永远的神 1.0, 永远滴神 0.8 (second source), 永远都是 0.5.
  Candidate didi{"永远滴神", 15, true, "yong'yuan'di'shen"};
  const std::vector<std::string> names{"baidu", "sogou"};
  Ranking ranking = RankCandidates({{yyds, generic}, {didi}}, both, "yyds", true, 3);
  if (ranking.mode != kAbbreviated || ranking.words.size() != 3 || ranking.words[0].text != yyds.text ||
      ranking.words[1].text != didi.text || ranking.ranks[1] != std::vector<int>{-1, 0}) return 24;
  RankPolicy loose;
  loose.threshold[kAbbreviated] = 0.3;
  if (RankCandidates({{yyds, generic, didi}}, {true}, "yyds", true, 3).words.size() != 2 ||
      RankCandidates({{yyds, generic, didi}}, {true}, "yyds", true, 3, loose).words.size() != 3) return 25;
  puts("PASS several cloud words when their scores are close; the threshold sets how many");
  auto hashOf = [](const std::vector<Candidate>& words) {
    std::vector<std::wstring> texts;
    for (const auto& w : words) texts.push_back(Utf8ToWide(w.text));
    return native::AnswerHash(texts);
  };
  const DWORD answer = hashOf(ranking.words);
  RankModel model;
  if (model.Choose(answer, 7, 1)) return 26;  // never served
  model.Served(names, ranking);
  if (model.Choose(answer, 5, 1)) return 27;  // word 1 was not shown
  for (int i = 0; i < 3; ++i)
    if (!model.Choose(answer, 7, 1)) return 28;
  RankPolicy policy = model.Policy(names);
  if (RankCandidates({{yyds, generic}, {didi}}, both, "yyds", true, 3, policy).words[0].text != didi.text) return 29;
  if (std::fabs(policy.weights[kFullPinyin][0] - 1.0) > 1e-9 || std::fabs(policy.weights[kFullPinyin][1] - 0.8) > 1e-9) return 30;
  puts("PASS three picks of the second source's word put it first; full pinyin learns separately");
  RankModel picky;  // never takes a further word
  picky.Served(names, ranking);
  for (int i = 0; i < 300; ++i) picky.Choose(answer, 7, 0);
  policy = picky.Policy(names);
  if (policy.threshold[kAbbreviated] < 0.9 || std::fabs(policy.threshold[kFullPinyin] - 0.5) > 1e-9 ||
      RankCandidates({{yyds, generic}, {didi}}, both, "yyds", true, 3, policy).words.size() != 1) return 31;
  RankModel eager;  // takes the third word a third of the time
  eager.Served(names, ranking);
  for (int i = 0; i < 100; ++i) eager.Choose(answer, 7, i % 3 ? 0 : 2);
  if (eager.Policy(names).threshold[kAbbreviated] > 0.4) return 32;
  // Full pinyin: the best word is local and hidden, so the shown one is the second (wangluo -> 王洛).
  Candidate network{"网络", 7, true, "wang'luo"}, name{"王洛", 7, true, "wang'luo"};
  Ranking homophones = RankCandidates({{network, name}, {network, name}}, both, "wangluo", true, 3);
  if (homophones.words.size() != 2 || homophones.mode != kFullPinyin) return 38;
  RankModel ignoring;
  ignoring.Served(names, homophones);
  for (int i = 0; i < 40; ++i) ignoring.Choose(hashOf(homophones.words), 2, -1);
  if (RankCandidates({{network, name}, {network, name}}, both, "wangluo", true, 3, ignoring.Policy(names)).words.size() != 1) return 39;
  puts("PASS further words are offered more when picked, less when ignored");
  // Most commits take a local word, so real pick rates are low. A source picked
  // more often than another must still lead (nihao: Sogou answers 你号 first).
  Ranking split = RankCandidates({{nihao, nihao2}, {nihao2, nihao}}, both, "nihao", true, 3);
  if (split.mode != kFullPinyin || split.words.size() != 2 || split.words[0].text != nihao.text) return 40;
  RankModel rare;
  rare.Served(names, split);
  for (int i = 0; i < 2000; ++i) rare.Choose(hashOf(split.words), 3, i % 50 == 49 ? 1 : -1);
  result = RankCandidates({{nihao, nihao2}, {nihao2, nihao}}, both, "nihao", true, 3, rare.Policy(names)).words;
  if (result.empty() || result[0].text != nihao2.text) return 41;
  puts("PASS a source picked one time in fifty keeps its learned lead");
  auto shared = std::make_shared<RankModel>();
  auto baidu = std::make_shared<FakeProvider>(1, true, std::vector<Candidate>{yyds, generic}, "baidu");
  auto sogou = std::make_shared<FakeProvider>(1, true, std::vector<Candidate>{didi}, "sogou");
  Engine adaptive({baidu, sogou}, 450, 4, shared);
  result = adaptive.QueryCandidates("yyds", true);
  if (result.size() != 3 || result[0].text != yyds.text) return 33;
  for (int i = 0; i < 3; ++i)
    if (!shared->Choose(hashOf(result), 7, 1)) return 34;
  result = adaptive.QueryCandidates("yyds", true);
  if (baidu->calls != 1 || result.empty() || result[0].text != didi.text) return 35;
  puts("PASS a cached answer is re-ranked with what was just learned");
  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring path = std::wstring(temp) + L"PinyinFluxRankingTest.ini";
  DeleteFileW(path.c_str());
  {
    RankModel saved(path);
    saved.Served(names, ranking);
    for (int i = 0; i < 3; ++i) saved.Choose(answer, 7, 1);
    saved.Save();
  }
  RankPolicy expected = model.Policy(names), loaded = RankModel(path).Policy(names);
  DeleteFileW(path.c_str());
  for (int m = 0; m < kRankModes; ++m) {
    if (std::fabs(expected.threshold[m] - loaded.threshold[m]) > 1e-3) return 36;
    for (size_t p = 0; p < names.size(); ++p)
      if (std::fabs(expected.weights[m][p] - loaded.weights[m][p]) > 1e-3) return 37;
  }
  puts("PASS learned counts survive a restart");
  // Observed (ldsd): the filter kept all three words, Microsoft Pinyin displayed
  // only the first. The others were never seen and must not count as ignored.
  const std::vector<std::wstring> answered{L"辣动时代", L"劳动所得", L"两点睡的"};
  native::Choice choice = native::ChoiceFor(answered, {L"陆地上的"}, 1, L"陆地上的");
  if (choice.shown != 1 || choice.picked != -1 || static_cast<DWORD>(choice.answer) != native::AnswerHash(answered)) return 42;
  if (native::ChoiceFor(answered, {}, 1, L"劳动所得").picked != -1) return 43;  // committed some other way
  // A local word is filtered out, so the next one takes the displayed slot.
  choice = native::ChoiceFor(answered, {L"辣动时代"}, 1, L"劳动所得");
  if (choice.shown != 2 || choice.picked != 1) return 44;
  if (native::ChoiceFor(answered, {}, 3, L"两点睡的").shown != 7 || native::ChoiceFor(answered, {}, 0, L"").shown != 0) return 45;
  puts("PASS only displayed cloud words count as seen; a filtered local word passes the slot on");
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
