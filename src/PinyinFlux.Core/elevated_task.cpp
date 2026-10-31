// SPDX-License-Identifier: MIT
#include "elevated_task.h"

#include "common.h"

#include <taskschd.h>
#include <wrl/client.h>

#include "log.h"

namespace pf {

namespace {

using Microsoft::WRL::ComPtr;

class Bstr {
 public:
  explicit Bstr(const std::wstring& s) : b_(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
  ~Bstr() { SysFreeString(b_); }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  operator BSTR() const { return b_; }

 private:
  BSTR b_;
};

class ComScope {
 public:
  ComScope() : ok_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
  ~ComScope() {
    if (ok_) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

 private:
  bool ok_;
};

// One task per Windows user: each runs the core in that user's session.
std::wstring TaskName() {
  wchar_t user[256 + 1];
  DWORD n = ARRAYSIZE(user);
  if (!GetUserNameW(user, &n)) return std::wstring(kAppName) + L" Core";
  return std::wstring(kAppName) + L" Core (" + user + L")";
}

ComPtr<ITaskService> Service() {
  ComPtr<ITaskService> service;
  if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))) return nullptr;
  VARIANT empty{};
  if (FAILED(service->Connect(empty, empty, empty, empty))) return nullptr;
  return service;
}

ComPtr<ITaskFolder> RootFolder(ITaskService* service) {
  ComPtr<ITaskFolder> folder;
  if (!service || FAILED(service->GetFolder(Bstr(L"\\"), &folder))) return nullptr;
  return folder;
}

std::wstring ActionPath(IRegisteredTask* task) {
  ComPtr<ITaskDefinition> definition;
  ComPtr<IActionCollection> actions;
  ComPtr<IAction> action;
  ComPtr<IExecAction> exec;
  BSTR path = nullptr;
  if (FAILED(task->get_Definition(&definition)) || FAILED(definition->get_Actions(&actions)) ||
      FAILED(actions->get_Item(1, &action)) || FAILED(action.As(&exec)) || FAILED(exec->get_Path(&path)) || !path)
    return {};
  std::wstring result(path, SysStringLen(path));
  SysFreeString(path);
  return result;
}

}  // namespace

bool IsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  bool elevated = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated;
  CloseHandle(token);
  return elevated;
}

void RegisterElevatedTask(const std::wstring& exe) {
  ComScope com;
  ComPtr<ITaskService> service = Service();
  ComPtr<ITaskFolder> folder = RootFolder(service.Get());
  ComPtr<ITaskDefinition> task;
  if (!folder || FAILED(service->NewTask(0, &task))) return;

  ComPtr<IRegistrationInfo> info;
  if (SUCCEEDED(task->get_RegistrationInfo(&info)))
    info->put_Description(Bstr(L"Starts PinyinFluxCore.exe with the rights native injection needs, without a UAC prompt. "
                               L"Has no triggers; PinyinFlux runs it on demand."));

  ComPtr<IPrincipal> principal;
  ComPtr<ITaskSettings> settings;
  ComPtr<IActionCollection> actions;
  ComPtr<IAction> action;
  ComPtr<IExecAction> exec;
  if (FAILED(task->get_Principal(&principal)) || FAILED(task->get_Settings(&settings)) || FAILED(task->get_Actions(&actions)) ||
      FAILED(actions->Create(TASK_ACTION_EXEC, &action)) || FAILED(action.As(&exec)))
    return;
  principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
  principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
  // The core is resident: no 72-hour limit, no battery rules, and normal
  // priority (the default 7 would also lower its I/O priority).
  settings->put_ExecutionTimeLimit(Bstr(L"PT0S"));
  settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
  settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
  settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
  settings->put_Priority(5);
  exec->put_Path(Bstr(exe));

  VARIANT empty{};
  ComPtr<IRegisteredTask> registered;
  HRESULT hr = folder->RegisterTaskDefinition(Bstr(TaskName()), task.Get(), TASK_CREATE_OR_UPDATE, empty, empty,
                                              TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered);
  if (FAILED(hr)) Log("elevated task: registration failed (0x%08lx)", static_cast<unsigned long>(hr));
}

bool StartElevatedTask(const std::wstring& exe) {
  ComScope com;
  ComPtr<ITaskService> service = Service();
  ComPtr<ITaskFolder> folder = RootFolder(service.Get());
  ComPtr<IRegisteredTask> task;
  if (!folder || FAILED(folder->GetTask(Bstr(TaskName()), &task))) return false;
  // A moved install keeps running unelevated until the user approves UAC
  // again, which re-registers the task for the new path.
  if (_wcsicmp(ActionPath(task.Get()).c_str(), exe.c_str()) != 0) return false;
  VARIANT none{};
  ComPtr<IRunningTask> running;
  HRESULT hr = task->Run(none, &running);
  if (FAILED(hr)) Log("elevated task: start failed (0x%08lx)", static_cast<unsigned long>(hr));
  return SUCCEEDED(hr);
}

bool RemoveElevatedTask() {
  ComScope com;
  ComPtr<ITaskService> service = Service();
  ComPtr<ITaskFolder> folder = RootFolder(service.Get());
  if (!folder) return true;
  HRESULT hr = folder->DeleteTask(Bstr(TaskName()), 0);
  return hr != E_ACCESSDENIED;
}

}  // namespace pf
