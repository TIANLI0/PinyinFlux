cmake_minimum_required(VERSION 3.24)
project(PinyinFlux LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Adding C must not replace the C++ compiler's correctly detected localized
# /showIncludes prefix. Otherwise Ninja silently loses header dependencies.
if(MSVC AND CMAKE_GENERATOR STREQUAL "Ninja")
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}")
endif()
# Static CRT: no VC++ redistributable needed, and a smaller resident footprint.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Optimize for size: the core is network-bound, so smaller code beats faster code.
string(REPLACE "/O2" "/O1" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

add_executable(PinyinFluxCore WIN32
  src/PinyinFlux.Core/main.cpp
  src/PinyinFlux.Core/elevated_task.cpp
  src/PinyinFlux.Core/common.cpp
  src/PinyinFlux.Core/log.cpp
  src/PinyinFlux.Core/config.cpp
  src/PinyinFlux.Core/json.cpp
  src/PinyinFlux.Core/http_client.cpp
  src/PinyinFlux.Core/engine.cpp
  src/PinyinFlux.Core/bing_format.cpp
  src/PinyinFlux.Core/cloud_query.cpp
  src/PinyinFlux.Core/providers/provider.cpp
  src/PinyinFlux.Core/providers/baidu.cpp
  src/PinyinFlux.Core/providers/sogou.cpp
  src/PinyinFlux.Core/cert.cpp
  src/PinyinFlux.Core/tls.cpp
  src/PinyinFlux.Core/sysproxy.cpp
  src/PinyinFlux.Core/client_filter.cpp
  src/PinyinFlux.Core/proxy_server.cpp
  src/PinyinFlux.Core/native_injector.cpp
  src/PinyinFlux.Core/native_symbols.cpp
  src/PinyinFlux.Core/typing_stats.cpp
  src/PinyinFlux.Core/PinyinFluxCore.rc
)
# The .rc is UTF-8 (Chinese file description).
set_source_files_properties(src/PinyinFlux.Core/PinyinFluxCore.rc PROPERTIES COMPILE_OPTIONS "/c65001")

target_compile_definitions(PinyinFluxCore PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(PinyinFluxCore PRIVATE /W4 /utf-8 /permissive- /EHsc $<$<CONFIG:Release>:/GL /Gy>)
target_link_options(PinyinFluxCore PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
# wininet is loaded on demand (see sysproxy.cpp); don't link it.
target_link_libraries(PinyinFluxCore PRIVATE ws2_32 winhttp crypt32 ncrypt secur32 iphlpapi dbghelp ole32 oleaut32 shell32 taskschd)

include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
FetchContent_MakeAvailable(minhook)
add_library(PinyinFluxInject SHARED src/PinyinFlux.Inject/native_hook.cpp)
target_compile_definitions(PinyinFluxInject PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(PinyinFluxInject PRIVATE /W4 /utf-8 /permissive- /EHsc /guard:cf)
target_link_options(PinyinFluxInject PRIVATE /guard:cf)
target_link_libraries(PinyinFluxInject PRIVATE winhttp ole32 minhook)
add_dependencies(PinyinFluxCore PinyinFluxInject)

include(CTest)
if(BUILD_TESTING)
  add_executable(PinyinFluxCloudPolicyTest tests/cloud_policy_test.cpp
    src/PinyinFlux.Core/engine.cpp src/PinyinFlux.Core/common.cpp src/PinyinFlux.Core/cloud_query.cpp
    src/PinyinFlux.Core/json.cpp src/PinyinFlux.Core/log.cpp src/PinyinFlux.Core/http_client.cpp
    src/PinyinFlux.Core/providers/provider.cpp src/PinyinFlux.Core/providers/baidu.cpp src/PinyinFlux.Core/providers/sogou.cpp)
  target_compile_definitions(PinyinFluxCloudPolicyTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(PinyinFluxCloudPolicyTest PRIVATE /W4 /utf-8 /EHsc)
  target_link_libraries(PinyinFluxCloudPolicyTest PRIVATE winhttp)
  add_test(NAME cloud_priority_deadline_cache COMMAND PinyinFluxCloudPolicyTest)
  # Resolves the native layout for this machine; downloads public symbols once.
  add_executable(PinyinFluxLayoutTest tests/native_layout_test.cpp src/PinyinFlux.Core/native_symbols.cpp
    src/PinyinFlux.Core/common.cpp src/PinyinFlux.Core/log.cpp src/PinyinFlux.Core/http_client.cpp)
  target_compile_definitions(PinyinFluxLayoutTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(PinyinFluxLayoutTest PRIVATE /W4 /utf-8 /EHsc)
  target_link_libraries(PinyinFluxLayoutTest PRIVATE winhttp dbghelp)
  add_test(NAME native_layout_from_symbols COMMAND PinyinFluxLayoutTest)
  set_tests_properties(native_layout_from_symbols PROPERTIES TIMEOUT 120)
  add_executable(PinyinFluxPlaceholderTest tests/native_placeholder_test.cpp src/PinyinFlux.Core/common.cpp src/PinyinFlux.Core/native_symbols.cpp src/PinyinFlux.Core/log.cpp src/PinyinFlux.Core/http_client.cpp)
  target_compile_definitions(PinyinFluxPlaceholderTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(PinyinFluxPlaceholderTest PRIVATE /W4 /utf-8 /EHsc)
  target_link_libraries(PinyinFluxPlaceholderTest PRIVATE runtimeobject ole32 winhttp dbghelp)
  add_dependencies(PinyinFluxPlaceholderTest PinyinFluxInject)
  add_test(NAME native_no_empty_placeholder COMMAND PinyinFluxPlaceholderTest $<TARGET_FILE:PinyinFluxInject>)
  add_executable(PinyinFluxNativeFormatTest tests/native_format_test.cpp
    src/PinyinFlux.Core/common.cpp src/PinyinFlux.Core/bing_format.cpp src/PinyinFlux.Core/cloud_query.cpp src/PinyinFlux.Core/json.cpp
    src/PinyinFlux.Core/native_symbols.cpp src/PinyinFlux.Core/log.cpp src/PinyinFlux.Core/http_client.cpp)
  target_compile_definitions(PinyinFluxNativeFormatTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(PinyinFluxNativeFormatTest PRIVATE /W4 /utf-8 /EHsc)
  target_link_libraries(PinyinFluxNativeFormatTest PRIVATE runtimeobject winhttp dbghelp)
  add_test(NAME native_response_parser COMMAND PinyinFluxNativeFormatTest)
  add_executable(PinyinFluxImeTest WIN32 EXCLUDE_FROM_ALL tests/ime_test_app.cpp)
  target_compile_definitions(PinyinFluxImeTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(PinyinFluxImeTest PRIVATE /W4 /utf-8 /EHsc)
  target_link_libraries(PinyinFluxImeTest PRIVATE ole32 oleaut32 shell32)
  add_executable(PinyinFluxNativeTest tests/native_hook_test.cpp src/PinyinFlux.Core/common.cpp src/PinyinFlux.Core/native_symbols.cpp src/PinyinFlux.Core/log.cpp src/PinyinFlux.Core/http_client.cpp)
  target_compile_definitions(PinyinFluxNativeTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(PinyinFluxNativeTest PRIVATE /W4 /utf-8 /EHsc)
  target_link_libraries(PinyinFluxNativeTest PRIVATE ws2_32 winhttp dbghelp)
  add_dependencies(PinyinFluxNativeTest PinyinFluxInject)
  add_test(NAME native_redirect_restore COMMAND PinyinFluxNativeTest $<TARGET_FILE:PinyinFluxInject>)
  add_test(NAME native_unknown_version COMMAND PinyinFluxNativeTest $<TARGET_FILE:PinyinFluxInject> --unsupported)
  add_test(NAME legacy_native_redirect_restore COMMAND PinyinFluxNativeTest $<TARGET_FILE:PinyinFluxInject> --legacy)
  add_test(NAME legacy_native_unknown_version COMMAND PinyinFluxNativeTest $<TARGET_FILE:PinyinFluxInject> --legacy --unsupported)
  add_test(NAME native_cached_connection COMMAND PinyinFluxNativeTest $<TARGET_FILE:PinyinFluxInject> --cached)
  add_test(NAME legacy_native_cached_connection COMMAND PinyinFluxNativeTest $<TARGET_FILE:PinyinFluxInject> --legacy --cached)
  set_tests_properties(native_redirect_restore native_unknown_version legacy_native_redirect_restore legacy_native_unknown_version native_cached_connection legacy_native_cached_connection PROPERTIES TIMEOUT 15)
  # Tests on the real Microsoft Pinyin images exit 77 (skipped) where the
  # Chinese IME is not installed, such as Windows Server CI runners.
  set_tests_properties(native_layout_from_symbols native_no_empty_placeholder native_response_parser
    native_redirect_restore native_unknown_version legacy_native_redirect_restore legacy_native_unknown_version
    native_cached_connection legacy_native_cached_connection PROPERTIES SKIP_RETURN_CODE 77)
endif()
