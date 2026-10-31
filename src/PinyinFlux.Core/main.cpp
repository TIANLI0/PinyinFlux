// PinyinFluxCore: answers Microsoft Pinyin's cloud candidate (qsonhs.aspx)
// requests locally from domestic cloud lexicons. Native mode injects
// PinyinFluxInject.dll into the input host; the legacy proxy mode intercepts
// the Bing requests through a PAC and a locally trusted certificate.
//
//   PinyinFluxCore.exe            run in the background (what autostart uses)
//   PinyinFluxCore.exe --console  run with log output in a console
//   PinyinFluxCore.exe --install  enable autostart and start (proxy mode: create + trust the certificate)
//   PinyinFluxCore.exe --uninstall stop, restore proxy settings, remove certificate + autostart
//   PinyinFluxCore.exe --stop     ask the running instance to exit
//   PinyinFluxCore.exe --remove-task  delete the elevated start task (run elevated by --uninstall)
//   PinyinFluxCore.exe --test <pinyin>  query the providers once
//   PinyinFluxCore.exe --dev      local test server; never touches system settings

#include "common.h"

#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "bing_format.h"
#include "cert.h"
#include "config.h"
#include "elevated_task.h"
#include "engine.h"
#include "log.h"
#include "native_injector.h"
#include "proxy_server.h"
#include "sysproxy.h"
#include "tls.h"

using namespace pf;

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kInternetSettingsKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\Connections";

enum ExitCode { kOk = 0, kUsage = 1, kNotPinyin = 2, kNotInstalled = 3, kListenFailed = 4, kCertFailed = 5 };

// Minimal argv splitter (quotes only) so we don't need shell32's CommandLineToArgvW.
std::vector<std::wstring> ParseArgs(const wchar_t* cmdline) {
  std::vector<std::wstring> args;
  std::wstring cur;
  bool quoted = false, any = false;
  for (const wchar_t* p = cmdline; *p; ++p) {
    if (*p == L'"') {
      quoted = !quoted;
      any = true;
    } else if ((*p == L' ' || *p == L'\t') && !quoted) {
      if (any) args.push_back(std::move(cur));
      cur.clear();
      any = false;
    } else {
      cur += *p;
      any = true;
    }
  }
  if (any) args.push_back(std::move(cur));
  return args;
}

// Output goes to an existing pipe/file when redirected (the settings app reads
// it that way), else to the parent's console. A console of our own is only
// created when asked for (--console / --dev), so verbs never flash a window.
void AttachConsoleOutput(bool allocate) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
  if (!AttachConsole(ATTACH_PARENT_PROCESS) && !(allocate && AllocConsole())) return;
  HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (con != INVALID_HANDLE_VALUE) SetStdHandle(STD_OUTPUT_HANDLE, con);
  SetConsoleOutputCP(CP_UTF8);
}

void Print(const std::string& text) {
  DWORD written;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

std::wstring ExePath() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  return std::wstring(path, n);
}

void SetAutostart(bool enable) {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
  if (enable) {
    std::wstring cmd = L"\"" + ExePath() + L"\"";
    RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                   static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kAppName);
  }
  RegCloseKey(key);
}

// Runs this executable with `args` after a UAC prompt and waits for it.
void RunElevated(const wchar_t* args) {
  std::wstring exe = ExePath();
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.lpVerb = L"runas";
  info.lpFile = exe.c_str();
  info.lpParameters = args;
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info) || !info.hProcess) return;
  WaitForSingleObject(info.hProcess, 30000);
  CloseHandle(info.hProcess);
}

bool SignalRunningInstance(const wchar_t* eventName) {
  HANDLE evt = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
  if (!evt) return false;
  SetEvent(evt);
  CloseHandle(evt);
  return true;
}

bool WaitForInstanceExit(DWORD timeoutMs) {
  for (DWORD waited = 0; waited < timeoutMs; waited += 100) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    if (!m) return true;
    CloseHandle(m);
    Sleep(100);
  }
  return false;
}

// Last resort when a running core did not honor the exit event: an uninstaller
// cannot delete an executable that is still running.
void TerminateOtherInstances() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32W entry{sizeof(entry)};
  for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
    if (entry.th32ProcessID == GetCurrentProcessId() || _wcsicmp(entry.szExeFile, L"PinyinFluxCore.exe") != 0) continue;
    if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID)) {
      TerminateProcess(p, 1);
      WaitForSingleObject(p, 2000);
      CloseHandle(p);
    }
  }
  CloseHandle(snap);
}

HANDLE g_exitEvent = nullptr;

BOOL WINAPI ConsoleCtrlHandler(DWORD) {
  if (g_exitEvent) SetEvent(g_exitEvent);
  return TRUE;
}

// Allows at most `limit` actions per `windowMs`, so we never get into a
// tug-of-war with another proxy tool that keeps rewriting the settings.
class RateLimiter {
 public:
  RateLimiter(int limit, ULONGLONG windowMs) : limit_(limit), windowMs_(windowMs) {}
  bool Allow() {
    ULONGLONG now = GetTickCount64();
    if (now - windowStart_ > windowMs_) {
      windowStart_ = now;
      count_ = 0;
    }
    return count_++ < limit_;
  }

 private:
  int limit_;
  ULONGLONG windowMs_;
  ULONGLONG windowStart_ = 0;
  int count_ = 0;
};

// Dev is a separate loopback instance (default port 18766), without injection
// or system proxy changes. Native mode requires no certificate.
int Run(bool console, bool dev, int devPort = 18766) {
  std::wstring mutexName = dev ? std::wstring(kMutexName) + L".Dev." + std::to_wstring(devPort) : kMutexName;
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (console) Print("PinyinFlux is already running.\n");
    return kOk;
  }

  auto config = LoadConfig();
  if (!dev && config->nativeInjection) {
    if (IsElevated()) {
      RegisterElevatedTask(ExePath());
    } else if (!console) {
      // Autostart and the settings app start us unelevated: hand over to the
      // task the user approved once, which runs the core elevated.
      ReleaseMutex(mutex);
      CloseHandle(mutex);
      if (StartElevatedTask(ExePath())) return kOk;
      mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
      if (GetLastError() == ERROR_ALREADY_EXISTS) return kOk;
    }
  }

  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  if (dev) {
    auto devConfig = std::make_shared<Config>(*config);
    devConfig->imeOnly = false;
    devConfig->nativeInjection = true;
    devConfig->port = devPort;
    devConfig->logToFile = false;
    config = devConfig;
  }
  SetCurrentConfig(config);
  LogInit(DataDir() + L"\\core.log", config->logToFile, console);
  Log("PinyinFlux %s starting%s", kVersion, dev ? " (dev mode)" : "");

  PCCERT_CONTEXT cert = config->nativeInjection ? nullptr : dev ? EnsureCertificate() : FindCertificate();
  if (!config->nativeInjection && (!cert || (!dev && !IsCertificateTrusted(cert)))) {
    // Without a trusted certificate the IME would just see TLS failures, so
    // leave the system untouched until --install has run.
    Log("certificate missing or not trusted; run PinyinFluxCore.exe --install");
    if (cert) CertFreeCertificateContext(cert);
    return kNotInstalled;
  }
  if (cert && !TlsInitServerCredentials(cert)) return kCertFailed;

  std::wstring exitName = dev ? std::wstring(kExitEventName) + L".Dev." + std::to_wstring(devPort) : kExitEventName;
  g_exitEvent = CreateEventW(nullptr, TRUE, FALSE, exitName.c_str());
  HANDLE reloadEvent = CreateEventW(nullptr, FALSE, FALSE, kReloadEventName);
  HANDLE regEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (console) SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  ProxyServer server;
  NativeInjector injector;
  server.SetEngine(std::make_shared<Engine>(*config));
  server.SetStatusExtra([&server, &injector, cert] {
    PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    std::string s = ",\"proxyApplied\":";
    s += IsSystemProxyApplied(server.Port()) ? "true" : "false";
    s += ",\"certTrusted\":";
    s += cert && IsCertificateTrusted(cert) ? "true" : "false";
    s += ",\"workingSetKB\":" + std::to_string(pmc.WorkingSetSize / 1024);
    if (CurrentConfig()->nativeInjection) s += injector.StatusJson();
    return s;
  });
  if (!server.Start(config->port)) return kListenFailed;
  if (!dev && config->nativeInjection) {
    RestoreSystemProxy();
    if (!injector.Start(config->port)) { Log("native: controller could not start"); return kListenFailed; }
  } else if (!dev) ApplySystemProxy(config->port);

  HKEY settingsKey = nullptr;
  if (!dev && !config->nativeInjection) RegOpenKeyExW(HKEY_CURRENT_USER, kInternetSettingsKey, 0, KEY_NOTIFY, &settingsKey);
  auto armRegistryWatch = [&] {
    if (settingsKey) RegNotifyChangeKeyValue(settingsKey, TRUE, REG_NOTIFY_CHANGE_LAST_SET, regEvent, TRUE);
  };
  armRegistryWatch();
  RateLimiter reclaimLimiter(3, 10 * 60 * 1000);

  // Hand idle pages back to the OS once a typing burst is over; they fault
  // back in cheaply on the next query.
  constexpr DWORD kTrimCheckMs = 60 * 1000;
  uint64_t lastSeenRequests = UINT64_MAX;
  auto trimIfIdle = [&] {
    uint64_t requests = server.Stats().cloudRequests.load() + server.Stats().tunnels.load();
    if (requests != lastSeenRequests) {
      lastSeenRequests = requests;
      SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }
  };
  trimIfIdle();

  HANDLE waits[] = {g_exitEvent, reloadEvent, regEvent};
  while (true) {
    DWORD r = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, kTrimCheckMs);
    if (r == WAIT_TIMEOUT) {
      trimIfIdle();
      continue;
    }
    if (r == WAIT_OBJECT_0) break;

    if (r == WAIT_OBJECT_0 + 1 && !dev) {
      auto next = LoadConfig();
      if (next->nativeInjection != CurrentConfig()->nativeInjection) {
        injector.Stop();
        RestoreSystemProxy();
        Log("connection mode changed; restart the core to apply it");
        break;
      }
      SetCurrentConfig(next);
      LogSetFileEnabled(next->logToFile);
      server.SetEngine(std::make_shared<Engine>(*next));
      if (next->port != server.Port()) {
        RestoreSystemProxy();
        server.Stop();
        if (!server.Start(next->port)) break;
        if (next->nativeInjection) injector.SetPort(next->port);
        else ApplySystemProxy(next->port);
      }
      Log("config reloaded");
    } else if (r == WAIT_OBJECT_0 + 2) {
      armRegistryWatch();
      if (CurrentConfig()->reclaimProxy && !IsSystemProxyApplied(server.Port())) {
        if (reclaimLimiter.Allow()) {
          Log("proxy settings were changed by another program; chaining them behind our PAC");
          ApplySystemProxy(server.Port());
        } else {
          Log("proxy settings keep changing; backing off");
        }
      }
    }
  }

  Log("shutting down");
  injector.Stop();
  if (!dev) RestoreSystemProxy();
  server.Stop();
  TlsFreeServerCredentials();
  if (cert) CertFreeCertificateContext(cert);
  if (settingsKey) RegCloseKey(settingsKey);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return kOk;
}

int Install() {
  LogInit({}, false, true);
  auto config = LoadConfig();
  if (!config->nativeInjection) {
    PCCERT_CONTEXT cert = EnsureCertificate();
    if (!cert) {
      Print("Failed to create the interception certificate.\n");
      return kCertFailed;
    }
    bool trusted = TrustCertificate(cert);
    CertFreeCertificateContext(cert);
    if (!trusted) {
      Print("The certificate was not trusted. Nothing changed.\n");
      return kCertFailed;
    }
  }
  SetAutostart(true);

  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + ExePath() + L"\"";
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    Print("Failed to start the core: " + std::to_string(GetLastError()) + "\n");
    return kListenFailed;
  }
  Print("PinyinFlux installed and started.\n");
  return kOk;
}

int Uninstall() {
  LogInit({}, false, true);
  SignalRunningInstance(kExitEventName);
  if (!WaitForInstanceExit(5000)) TerminateOtherInstances();
  RestoreSystemProxy();  // in case the instance was killed rather than stopped
  SetAutostart(false);
  RemoveCertificate();
  if (!RemoveElevatedTask()) RunElevated(L"--remove-task");
  Print("PinyinFlux removed. Proxy settings restored.\n");
  return kOk;
}

int RunTest(const std::string& input) {
  LogInit({}, false, true);
  auto config = LoadConfig();
  SetCurrentConfig(config);
  std::string pinyin = NormalizePinyin(input);
  if (pinyin.empty()) {
    Print("not pinyin: " + input + "\n");
    return kNotPinyin;
  }
  Engine engine(*config);
  Print(BuildBingResponse(pinyin, engine.Query(pinyin)) + "\n");
  return kOk;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::vector<std::wstring> argv = ParseArgs(GetCommandLineW());
  size_t argc = argv.size();
  std::wstring cmd = argc > 1 ? argv[1] : L"";

  if (cmd.empty() || cmd == L"--run") return Run(false, false);

  bool wantsConsole = cmd == L"--console" || cmd == L"--dev";
  AttachConsoleOutput(wantsConsole);
  if (cmd == L"--console") return Run(true, false);
  if (cmd == L"--dev") {
    int port = argc > 2 ? _wtoi(argv[2].c_str()) : 18766;
    if (port < 1024 || port > 65535) return kUsage;
    return Run(true, true, port);
  }
  if (cmd == L"--install") return Install();
  if (cmd == L"--uninstall") return Uninstall();
  if (cmd == L"--stop") {
    // Waits, so an installer can replace the executable right after this returns.
    if (!SignalRunningInstance(kExitEventName)) return kOk;
    if (!WaitForInstanceExit(5000)) TerminateOtherInstances();
    RestoreSystemProxy();
    return kOk;
  }
  if (cmd == L"--remove-task") return RemoveElevatedTask() ? kOk : kUsage;
  if (cmd == L"--test" && argc > 2) return RunTest(WideToUtf8(argv[2]));

  Print("usage: PinyinFluxCore.exe [--console | --install | --uninstall | --stop | --test <pinyin>]\n");
  return kUsage;
}
