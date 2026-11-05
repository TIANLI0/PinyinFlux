// SPDX-License-Identifier: MIT
// Resolves the native layout from public symbols and instruction patterns. On
// the build first verified by hand it must reproduce those offsets exactly;
// elsewhere every required field must resolve. An argument names a folder
// holding InputService.dll, ChsProxyDS.dll and ChsEM.dll of another build.
#include "../src/PinyinFlux.Core/native_symbols.h"
#include <cstdio>

using namespace pf;
int failures = 0;
void Check(bool ok, const char* what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

int wmain(int argc, wchar_t** argv) {
  native::NativeLayout layout;
  LayoutReport report;
  std::string summary = ResolveNativeLayout(layout, &report, argc > 1 ? argv[1] : L"");
  printf("layout: %s (InputService %s, %s)\n", summary.c_str(), report.version.c_str(), report.usable ? "usable" : "not usable");
  const auto& e = layout.engine;
  const auto& p = layout.proxy;
  if (!e.image.stamp) { puts("SKIP InputService.dll not available"); return 77; }  // CTest skip code
  printf("engine %08lx/%lx: changed=%lx rule=%lx preload=%lx excerpts@%lx desktop=%lx flags=%lx,%lx,%lx settings=%lx "
         "slots=%lx,%lx,%lx,%lx pool=%lx spelling=%lx cloudBase=%lx text=%lx\n",
         e.image.stamp, e.image.size, e.pinyinChanged.rva, e.triggerRule.rva, e.preloading.rva, e.excerpts,
         e.desktopChanged.rva, e.ruleFlags[0], e.ruleFlags[1], e.ruleFlags[2], e.ruleSettings, e.settingsSlots[0],
         e.settingsSlots[1], e.settingsSlots[2], e.settingsSlots[3], e.rulePool, e.poolSpelling, e.cloudBase, e.candidateText);
  printf("engine optional: setting=%lx candidate=%lx query=%lx request=%lx ready=%lx filter=%lx spell=%lx order=%lx commit=%lx\n",
         e.settingRule.rva, e.candidateRule.rva, e.doCloudQuery.rva, e.requestSuggestion.rva, e.suggestionReady.rva,
         e.filterSuggestions.rva, e.spellQuery.rva, e.displayOrder.rva, e.commitText.rva);
  printf("proxy %08lx/%lx: cache=%lx response=%lx debounce=%lx lock=%lx; legacy %08lx: changed=%lx\n", p.image.stamp,
         p.image.size, p.localCache.rva, p.addResponse.rva, p.debounce.rva, p.requestLock, layout.legacy.image.stamp,
         layout.legacy.cloudChanged.rva);
  Check(e.pinyinChanged.rva && e.triggerRule.rva && e.preloading.rva && e.setExcerpts.rva && e.desktopChanged.rva &&
            e.listClass && e.listInterface, "required engine functions resolve");
  Check(report.usable, "required fields extracted from code");
  if (e.image == native::ImageId{0x78148071, 0x4d9000}) {
    Check(e.pinyinChanged.rva == 0x1083b0 && e.triggerRule.rva == 0x106d3c && e.preloading.rva == 0x2bc30c &&
              e.setExcerpts.rva == 0x29aa00 && e.desktopChanged.rva == 0x10a110 && e.listClass == 0x412770 &&
              e.listInterface == 0x412760 && e.cloudChanged.rva == 0x2048b0 && e.settingRule.rva == 0x203698 &&
              e.candidateRule.rva == 0x202f08 && e.commitText.rva == 0x29aae0,
          "verified build: functions match the disassembly");
    Check(e.excerpts == 0x278 && e.ruleFlags[0] == 0x228 && e.ruleFlags[1] == 0x22b && e.ruleFlags[2] == 0x22c &&
              e.ruleSettings == 0x1f8 && e.settingsSlots[0] == 0x278 && e.settingsSlots[1] == 0xd8 &&
              e.settingsSlots[2] == 0xb8 && e.settingsSlots[3] == 0xc8 && e.rulePool == 0x1f0 && e.poolSpelling == 0x28 &&
              e.cloudBase == 0x130 && e.candidateText == 0x0a,
          "verified build: field offsets extracted from code match");
  }
  // Windows 11 22H2 10.0.22621.4111 (KB5041587), compiled with XFG; checked by
  // disassembly. Run with a folder holding its images from the symbol server.
  if (e.image == native::ImageId{0x72e917f2, 0x4db000}) {
    Check(e.pinyinChanged.rva == 0x2cbdd0 && e.triggerRule.rva == 0x2c9cb0 && e.preloading.rva == 0x2ca3f8 &&
              e.desktopChanged.rva == 0xc8450 && e.cloudChanged.rva == 0x1e3bd0,
          "22621.4111: functions match the disassembly");
    Check(e.excerpts == 0x278 && e.ruleFlags[0] == 0x228 && e.ruleFlags[1] == 0x22b && e.ruleFlags[2] == 0x22c &&
              e.ruleSettings == 0x1f8 && e.settingsSlots[0] == 0x278 && e.settingsSlots[1] == 0xd8 &&
              e.settingsSlots[2] == 0xb8 && e.settingsSlots[3] == 0xc8 && e.rulePool == 0x1f0 && e.poolSpelling == 0x28 &&
              e.cloudBase == 0x130 && e.candidateText == 0x0a,
          "22621.4111: field offsets extracted from XFG code match");
  }
  if (p.image == native::ImageId{0x34691f04, 0x53000})
    Check(p.getCandidates.rva == 0x146e8 && p.localCache.rva == 0x15e20 && p.addCloud.rva == 0x11154 &&
              p.addResponse.rva == 0x11888 && p.debounce.rva == 0x16120 && p.requestLock == 0x4c578,
          "verified build: ChsProxyDS functions and request lock match");
  if (layout.legacy.image == native::ImageId{0x2fa8d09e, 0x109000})
    Check(layout.legacy.cloudChanged.rva == 0x6d930, "verified build: legacy ChsEM handler matches");
  return failures ? 1 : 0;
}
