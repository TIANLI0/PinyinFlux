#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pf {

inline constexpr wchar_t kAppName[] = L"PinyinFlux";
inline constexpr char kVersion[] = "0.1.1";

// Named kernel objects shared with the settings UI.
inline constexpr wchar_t kMutexName[] = L"Local\\PinyinFlux.Core";
inline constexpr wchar_t kReloadEventName[] = L"Local\\PinyinFlux.Reload";
inline constexpr wchar_t kExitEventName[] = L"Local\\PinyinFlux.Exit";

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);

std::string UrlDecode(std::string_view s);
std::string UrlEncode(std::string_view s);

std::string ToLowerAscii(std::string_view s);
bool IEquals(std::string_view a, std::string_view b);
bool IStartsWith(std::string_view s, std::string_view prefix);
std::string_view Trim(std::string_view s);
std::vector<std::string> Split(std::string_view s, char sep);

// %LOCALAPPDATA%\PinyinFlux, created on first use.
const std::wstring& DataDir();

// Returns the value of `key` in a URL query string ("a=1&q=x"), URL-decoded.
std::string QueryParam(std::string_view query, std::string_view key);

}  // namespace pf
