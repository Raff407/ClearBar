// Shared-memory protocol between ClearBar.exe (single writer) and
// ClearBarTap.dll inside explorer.exe (reader). Local-session objects only;
// nothing leaves the machine.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace cb {

inline constexpr uint32_t kMagic = 0x42524C43;  // 'CLRB'
inline constexpr uint32_t kProtocolVersion = 2;  // v2: colours removed, blur added
inline constexpr int kMaxTaskbars = 16;

// Versioned with the protocol so an older TAP can never read newer bytes
// (or look "ready" to a newer exe).
inline constexpr wchar_t kMapName[] = L"Local\\ClearBar.State.v2";
inline constexpr wchar_t kChangedEventName[] = L"Local\\ClearBar.Changed.v2";
// + explorer PID. Manual-reset; set by the TAP once it is watching the tree.
inline constexpr wchar_t kReadyEventPrefix[] = L"Local\\ClearBar.TapReady.v2.";
// Ready event of an older (v1) TAP: used only to explain why injection fails.
inline constexpr wchar_t kLegacyReadyEventPrefix[] = L"Local\\ClearBar.TapReady.v1.";
// + explorer PID. Created only inside explorer; guards against double install.
// Deliberately NOT versioned: two TAP generations must never run in one explorer.
inline constexpr wchar_t kInstalledMutexPrefix[] = L"Local\\ClearBar.TapInstalled.v1.";
inline constexpr wchar_t kWorkerClass[] = L"ClearBar.Worker";
// Registered message the TAP posts to the worker when it finds a taskbar
// that has no entry yet (e.g. a monitor was just attached).
inline constexpr wchar_t kTaskbarFoundMsg[] = L"ClearBar.TaskbarFound.v1";

// Normal = Windows' own background, Clear = fully see-through,
// Blur = see-through with a Gaussian blur of what is behind the taskbar.
enum class Kind : uint32_t { Normal = 0, Clear = 1, Blur = 2, Count = 3 };

inline constexpr uint32_t kMinBlur = 1, kMaxBlur = 50, kDefaultBlur = 8;

struct Appearance {
  uint32_t kind;      // Kind
  uint32_t blur;      // blur strength (Gaussian std-dev, px) - only for Kind::Blur
  uint32_t showLine;  // 0/1: taskbar top border line (Windows 11 only)
  uint32_t reserved;
};

inline bool operator==(const Appearance& a, const Appearance& b) {
  return a.kind == b.kind && a.blur == b.blur && a.showLine == b.showLine;
}
inline bool operator!=(const Appearance& a, const Appearance& b) { return !(a == b); }

struct Entry {
  uint64_t hwnd;  // Shell_TrayWnd / Shell_SecondaryTrayWnd
  Appearance app;
};

inline constexpr uint32_t kFlagDebugLog = 1u;
inline constexpr uint32_t kFlagTraceTree = 2u;

struct Shared {
  uint32_t magic;
  uint32_t version;
  volatile LONG seq;    // seqlock: odd while the writer is mid-update
  uint32_t exePid;
  uint32_t generation;  // bumped to force a full re-apply (resume, unlock, ...)
  uint32_t flags;
  uint32_t count;
  uint32_t pad;
  Entry entries[kMaxTaskbars];
};

// ---- seqlock (single writer, many readers; reader maps read-only) ----
inline void WriteBegin(Shared* s) { InterlockedIncrement(&s->seq); }
inline void WriteEnd(Shared* s) { InterlockedIncrement(&s->seq); }

inline bool ReadSnapshot(const Shared* s, Shared* out) {
  for (int i = 0; i < 4096; ++i) {
    const LONG a = ReadAcquire(&s->seq);
    if (a & 1) { YieldProcessor(); continue; }
    memcpy(out, const_cast<const Shared*>(s), sizeof(Shared));
    MemoryBarrier();
    const LONG b = ReadAcquire(&s->seq);
    if (a == b) return true;
  }
  return false;
}

inline bool Validate(const Shared& s) {
  if (s.magic != kMagic || s.version != kProtocolVersion) return false;
  if (s.count > static_cast<uint32_t>(kMaxTaskbars)) return false;
  for (uint32_t i = 0; i < s.count; ++i) {
    if (s.entries[i].app.kind >= static_cast<uint32_t>(Kind::Count)) return false;
    if (s.entries[i].app.showLine > 1) return false;
    if (s.entries[i].app.blur > kMaxBlur) return false;
  }
  return true;
}

inline const Appearance* FindEntry(const Shared& s, uint64_t hwnd) {
  for (uint32_t i = 0; i < s.count && i < static_cast<uint32_t>(kMaxTaskbars); ++i)
    if (s.entries[i].hwnd == hwnd) return &s.entries[i].app;
  return nullptr;
}

// ---- Windows version -> which taskbar backend ----
// Windows 11 (build 22000+) draws the taskbar with XAML: the XAML diagnostics
// path (ClearBarTap.dll in Explorer). Windows 10 ("classic") draws it with DWM:
// ClearBar.exe sets the accent policy on the taskbar windows itself (no DLL).
// Build 0 (version unknown) behaves exactly like Windows 11, as in v1.0.
inline constexpr uint32_t kFirstXamlTaskbarBuild = 22000;
inline constexpr uint32_t kMinClassicBuild = 19041;  // Windows 10 2004
inline constexpr uint32_t kMaxClassicBuild = 19045;  // Windows 10 22H2 (last client feature update)
inline constexpr uint8_t kProductWorkstation = 1;    // VER_NT_WORKSTATION

inline bool IsClassicTaskbarBuild(uint32_t build) { return build != 0 && build < kFirstXamlTaskbarBuild; }
// Windows 10 client 2004-22H2 (incl. LTSC 2021). Insider builds 19046-21999 and
// Windows Server (e.g. 2022 = 20348) are not supported.
inline bool IsSupportedClassic(uint32_t build, uint8_t productType) {
  return build >= kMinClassicBuild && build <= kMaxClassicBuild && productType == kProductWorkstation;
}
// Any build ClearBar runs on: Windows 11 (or unknown), or a supported Windows 10.
// The product type (Server refusal) is checked only in the classic range: for
// 22000+ the v1.0 rule stays as it was (any edition, install.ps1 included), so
// Windows Server 2025 (26100) keeps the v1.0 behaviour and Server 2022 (20348)
// is refused. install.ps1 must apply the same rule.
inline bool IsSupportedBuild(uint32_t build, uint8_t productType) {
  return !IsClassicTaskbarBuild(build) || IsSupportedClassic(build, productType);
}

struct OsInfo {
  uint32_t build;
  uint8_t productType;
};
// A compatibility layer (e.g. "Windows 8" mode, or one inherited from the parent
// process) makes RtlGetVersion report an older Windows. The binaries cannot load
// on anything before Windows 10, so major < 10 is always such a lie: treat the
// build as unknown (0), which behaves exactly like Windows 11, as v1.0 did.
inline uint32_t SanitizeBuild(uint32_t major, uint32_t build) { return major < 10 ? 0 : build; }
// RtlGetVersion, not GetVersionEx: the binaries have no compatibility manifest,
// so GetVersionEx would report 6.2. Returns {0, 0} on failure.
inline OsInfo QueryOs() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  OsInfo info{0, 0};
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return info;
  auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
  if (!fn) return info;
  OSVERSIONINFOEXW v{};
  v.dwOSVersionInfoSize = sizeof(v);
  if (fn(&v) != 0) return info;
  info.build = SanitizeBuild(v.dwMajorVersion, v.dwBuildNumber);
  info.productType = v.wProductType;
  return info;
}

}  // namespace cb
