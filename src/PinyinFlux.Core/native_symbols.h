#pragma once

#include <memory>
#include <string>

#include "../PinyinFlux.Inject/protocol.h"

namespace pf {

// Public symbols of one system image: its PDB is downloaded once from the
// Microsoft symbol server into DataDir()\symbols and read with dbghelp. For
// the build PinyinFlux was first verified on, a built-in table is used when
// the download is unavailable.
class SymbolImage {
 public:
  ~SymbolImage();
  bool Open(const std::wstring& path);
  native::ImageId Id() const { return id_; }
  // 0 when unknown.
  DWORD Rva(const char* name);
  // Code or data of the image as loaded (sections at their RVAs).
  const BYTE* At(DWORD rva) const { return rva && rva < id_.size ? base_ + rva : nullptr; }
  bool Fill(native::Function& function, const char* name);
  const std::string& Source() const { return source_; }

 private:
  HMODULE module_ = nullptr;
  const BYTE* base_ = nullptr;
  std::wstring path_, name_;
  native::ImageId id_;
  HANDLE symbols_ = nullptr;
  DWORD64 loadedBase_ = 0;
  std::string source_;  // "symbols", "built-in" or empty
};

struct LayoutReport {
  // The candidate engine (InputService, or legacy ChsEM) resolved completely:
  // the extension can work with this layout.
  bool usable = false;
  // An installed image has no public symbols here yet (the download failed,
  // usually for want of a network right after a Windows update): retry later
  // rather than giving up on this build.
  bool missingSymbols = false;
  std::string version;  // InputService.dll file version, e.g. "10.0.26100.9767"
};

// Resolves everything the extension needs for the installed images. Returns
// a summary for the status page ("symbols", "built-in", or what is missing).
std::string ResolveNativeLayout(native::NativeLayout& layout, LayoutReport* report = nullptr);

// TimeDateStamp and SizeOfImage of an image file on disk (zero when unreadable).
native::ImageId ImageOnDisk(const std::wstring& path);

}  // namespace pf
