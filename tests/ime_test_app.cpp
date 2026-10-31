// SPDX-License-Identifier: MIT
// Disposable, empty input surface for manual/native-IME end-to-end validation.
// With --candidates <file>, every candidate list the IME exposes through TSF
// (ITfCandidateListUIElement) is written to <file> as "<sequence>\t<a>|<b>|...".
// Reading the list never commits text, so automated runs leave the user's
// self-learning untouched.
#include <windows.h>
#include <shellapi.h>
#include <msctf.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;
HWND edit = nullptr;
std::wstring candidateFile;

class CandidateSink final : public ITfUIElementSink {
 public:
  explicit CandidateSink(ComPtr<ITfUIElementMgr> manager) : manager_(std::move(manager)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (id == IID_IUnknown || id == IID_ITfUIElementSink) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { ULONG left = --references_; if (!left) delete this; return left; }
  HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD id, BOOL* show) override { *show = TRUE; Write(id); return S_OK; }
  HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id) override { Write(id); return S_OK; }
  HRESULT STDMETHODCALLTYPE EndUIElement(DWORD) override { Save(L""); return S_OK; }

 private:
  void Write(DWORD id) {
    ComPtr<ITfUIElement> element;
    ComPtr<ITfCandidateListUIElement> list;
    if (FAILED(manager_->GetUIElement(id, &element)) || FAILED(element.As(&list))) return;
    UINT count = 0;
    if (FAILED(list->GetCount(&count))) return;
    std::wstring line;
    for (UINT i = 0; i < count && i < 64; ++i) {
      BSTR text = nullptr;
      if (SUCCEEDED(list->GetString(i, &text)) && text) {
        if (i) line += L'|';
        line.append(text, SysStringLen(text));
        SysFreeString(text);
      }
    }
    Save(line);
  }
  void Save(const std::wstring& line) {
    std::wstring record = std::to_wstring(++sequence_) + L"\t" + line;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, record.c_str(), static_cast<int>(record.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, record.c_str(), static_cast<int>(record.size()), utf8.data(), bytes, nullptr, nullptr);
    std::wstring temporary = candidateFile + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
    MoveFileExW(temporary.c_str(), candidateFile.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  ComPtr<ITfUIElementMgr> manager_;
  ULONG references_ = 1;
  ULONG sequence_ = 0;
};

LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
          ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 20, 48, 650, 260, window, nullptr, nullptr, nullptr);
      SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
      CreateWindowW(L"STATIC", L"PinyinFlux 原生候选测试：在下方输入拼音，检查云候选并选词上屏。", WS_CHILD | WS_VISIBLE,
          20, 14, 650, 28, window, nullptr, nullptr, nullptr);
      return 0;
    case WM_SETFOCUS: SetFocus(edit); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i + 1 < argc; ++i)
    if (wcscmp(argv[i], L"--candidates") == 0) candidateFile = argv[i + 1];
  if (argv) LocalFree(argv);
  ComPtr<ITfThreadMgr> threads;
  ComPtr<ITfSource> source;
  DWORD cookie = TF_INVALID_COOKIE;
  if (!candidateFile.empty() &&
      SUCCEEDED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&threads))) &&
      SUCCEEDED(threads.As(&source))) {
    ComPtr<ITfUIElementMgr> manager;
    if (SUCCEEDED(threads.As(&manager))) {
      auto* sink = new CandidateSink(manager);
      source->AdviseSink(IID_ITfUIElementSink, sink, &cookie);
      sink->Release();
    }
  }
  WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = Window;
  cls.lpszClassName = L"PinyinFlux.ImeTest"; cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
  cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); RegisterClassW(&cls);
  HWND window = CreateWindowW(cls.lpszClassName, L"PinyinFlux IME Test", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, 720, 380, nullptr, nullptr, instance, nullptr);
  ShowWindow(window, show); SetFocus(edit);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  if (source && cookie != TF_INVALID_COOKIE) source->UnadviseSink(cookie);
  source.Reset(); threads.Reset();
  CoUninitialize(); return 0;
}
