// SPDX-License-Identifier: MIT
// The compatibility check must not depend on the user's Microsoft Pinyin
// settings. With 双拼 on, the extension deliberately leaves cloud requests
// alone, and the check's redirect step once failed on every such machine.
// This process's HKCU is replaced by a volatile scratch key with 双拼 on.
#include "../src/PinyinFlux.Core/native_compat.h"

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return 2;
  const wchar_t* scratch = L"Software\\PinyinFlux\\PreflightTest";
  HKEY hive = nullptr, chs = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, scratch, 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &hive, nullptr) ||
      RegCreateKeyExW(hive, L"Software\\Microsoft\\InputMethod\\Settings\\CHS", 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                      nullptr, &chs, nullptr))
    return 3;
  DWORD on = 1;
  RegSetValueExW(chs, L"Enable Double Pinyin", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&on), sizeof(on));
  RegCloseKey(chs);
  if (RegOverridePredefKey(HKEY_CURRENT_USER, hive)) return 3;
  int result = pf::RunPreflight(argv[1]);
  RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
  RegCloseKey(hive);
  RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
  RegDeleteKeyW(HKEY_CURRENT_USER, scratch);
  return result;
}
