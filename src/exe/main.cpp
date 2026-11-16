// ClearBar.exe - tiny, silent, event-driven controller.
//  * no UI, no tray icon, no network, no polling
//  * reads config.ini next to the exe (hot reload on save)
//  * decides each taskbar's appearance from the active "dynamic mode"
//  * publishes it to ClearBarTap.dll (inside explorer) via shared memory
//
// Command line:  ClearBar.exe            run (no-op if already running)
//                ClearBar.exe --exit     stop the running instance (taskbar restores)
//                ClearBar.exe --reload   force a config reload + full re-apply
#include <windows.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include "logic.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "wtsapi32.lib")

using namespace cb;

namespace {

constexpr UINT_PTR kTimerRecompute = 1;
constexpr UINT_PTR kTimerReset = 2;
constexpr UINT_PTR kTimerInjectTimeout = 3;
constexpr UINT_PTR kTimerInjectRetry = 4;
constexpr UINT_PTR kTimerReload = 5;
constexpr UINT kMsgReload = WM_APP + 1;

constexpr UINT kRecomputeDelayMs = 15;
constexpr UINT kInjectTimeoutMs = 35000;
constexpr int kMaxInjectFailures = 5;

const wchar_t kDefaultConfig[] =
    L"; ClearBar configuration. Saving this file applies changes instantly.\r\n"
    L";\r\n"
    L"; accent    = clear | blur | normal\r\n"
    L";             clear  - fully see-through\r\n"
    L";             blur   - see-through with a soft blur of what is behind the taskbar\r\n"
    L";             normal - Windows' own taskbar background\r\n"
    L"; blur      = 1-50   blur strength for accent = blur (default 8)\r\n"
    L"; show_line = true | false   (the thin line along the top edge of the Windows 11 taskbar)\r\n"
    L"; enabled   = true | false   (desktop is always enabled)\r\n"
    L";\r\n"
    L"; Priority when several apply: task_view_opened > start_opened > search_opened >\r\n"
    L";                              maximized_window > visible_window > desktop\r\n"
    L"; Note: on Windows 11 26xxx the Start menu is hosted by SearchHost, so opening\r\n"
    L";       Start triggers search_opened.\r\n"
    L"\r\n"
    L"[general]\r\n"
    L"debug_log = false\r\n"
    L"\r\n"
    L"[desktop]\r\n"
    L"accent = clear\r\n"
    L"blur = 8\r\n"
    L"show_line = false\r\n"
    L"\r\n"
    L"[visible_window]\r\n"
    L"enabled = false\r\n"
    L"accent = clear\r\n"
    L"blur = 8\r\n"
    L"show_line = false\r\n"
    L"\r\n"
    L"[maximized_window]\r\n"
    L"enabled = false\r\n"
    L"accent = blur\r\n"
    L"blur = 8\r\n"
    L"show_line = true\r\n"
    L"\r\n"
    L"[start_opened]\r\n"
    L"enabled = false\r\n"
    L"accent = normal\r\n"
    L"show_line = true\r\n"
    L"\r\n"
    L"[search_opened]\r\n"
    L"enabled = false\r\n"
    L"accent = normal\r\n"
    L"show_line = true\r\n"
    L"\r\n"
    L"[task_view_opened]\r\n"
    L"enabled = true\r\n"
    L"accent = normal\r\n"
    L"show_line = true\r\n";

struct Taskbar {
  HWND hwnd;
  HMONITOR mon;
  RECT monRect;
};

struct TrackedWindow {
  HWND hwnd;
  HMONITOR mon;
  bool zoomed;
};

struct App {
  HWND wnd = nullptr;
  UINT msgTaskbarCreated = 0;
  UINT msgTaskbarFound = 0;
  std::wstring dir, configPath, logPath, tapPath;
  std::wstring configText;
  Config cfg = DefaultConfig();

  HANDLE mapping = nullptr;
  Shared* shared = nullptr;
  HANDLE changed = nullptr;
  HANDLE dirWatch = INVALID_HANDLE_VALUE;
  uint32_t generation = 0;

  std::vector<Taskbar> taskbars;
  std::vector<Appearance> lastWritten;
  bool lastValid = false;
  std::vector<TrackedWindow> tracked;

  DWORD explorerPid = 0;
  uint64_t explorerCreated = 0;  // pid + creation time identify an explorer instance
  HANDLE ready = nullptr;  // TapReady event for explorerPid
  bool tapReady = false;
  HHOOK hook = nullptr;
  HMODULE tapModule = nullptr;
  int injectFailures = 0;
  int xamlWaits = 0;

  std::vector<HWINEVENTHOOK> eventHooks;
  bool shellHooks = false, windowHooks = false;
};
App g;

// ------------------------------------------------------------------ log
void Log(const wchar_t* fmt, ...) {
  if (!(g.cfg.flags & kFlagDebugLog) || g.logPath.empty()) return;
  wchar_t buf[1024];
  SYSTEMTIME st;
  GetLocalTime(&st);
  int n = swprintf_s(buf, L"%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  if (n < 0) n = 0;
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf + n, _countof(buf) - n - 3, _TRUNCATE, fmt, ap);
  va_end(ap);
  wcscat_s(buf, L"\r\n");
  char utf8[3072];
  const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
  if (len <= 1) return;
  HANDLE f = CreateFileW(g.logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(f, &size) && size.QuadPart > 2 * 1024 * 1024) {
    CloseHandle(f);  // cap the log; start over
    f = CreateFileW(g.logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
  }
  DWORD w;
  WriteFile(f, utf8, static_cast<DWORD>(len - 1), &w, nullptr);
  CloseHandle(f);
}

// ------------------------------------------------------------ helpers
std::wstring ProcessImagePath(DWORD pid) {
  std::wstring out;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return out;
  wchar_t buf[MAX_PATH * 2];
  DWORD n = _countof(buf);
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
  CloseHandle(p);
  return out;
}

std::wstring FileNameOf(const std::wstring& path) {
  const size_t s = path.find_last_of(L"\\/");
  return s == std::wstring::npos ? path : path.substr(s + 1);
}

bool IsRealExplorer(DWORD pid) {
  wchar_t win[MAX_PATH];
  const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  std::wstring expected = std::wstring(win) + L"\\explorer.exe";
  return IEquals(ProcessImagePath(pid), expected);
}

std::wstring ClassOf(HWND h) {
  wchar_t buf[256];
  const int n = GetClassNameW(h, buf, _countof(buf));
  return n > 0 ? std::wstring(buf, n) : std::wstring();
}

std::wstring TitleOf(HWND h) {
  wchar_t buf[256];
  const int n = GetWindowTextW(h, buf, _countof(buf));
  return n > 0 ? std::wstring(buf, n) : std::wstring();
}

bool IsCloaked(HWND h) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

// ------------------------------------------------------------- config
bool ReadFileText(const std::wstring& path, std::wstring* out) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(f, &size) || size.QuadPart > 256 * 1024) { CloseHandle(f); return false; }
  std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const BOOL ok = bytes.empty() || ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  CloseHandle(f);
  if (!ok) return false;
  bytes.resize(read);
  out->clear();
  if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xFE) {  // UTF-16LE
    out->assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    return true;
  }
  size_t skip = (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
  if (bytes.size() == skip) return true;
  const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, static_cast<int>(bytes.size() - skip), nullptr, 0);
  if (n <= 0) return false;
  out->resize(n);
  MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, static_cast<int>(bytes.size() - skip), out->data(), n);
  return true;
}

void WriteDefaultConfigIfMissing() {
  if (GetFileAttributesW(g.configPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  HANDLE f = CreateFileW(g.configPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  char utf8[8192];
  const int len = WideCharToMultiByte(CP_UTF8, 0, kDefaultConfig, -1, utf8, sizeof(utf8), nullptr, nullptr);
  DWORD w;
  if (len > 1) WriteFile(f, utf8, static_cast<DWORD>(len - 1), &w, nullptr);
  CloseHandle(f);
}

// Returns true if the effective config changed.
bool LoadConfig(bool force) {
  std::wstring text;
  if (!ReadFileText(g.configPath, &text)) {
    if (!force) return false;  // keep current settings (file mid-save / deleted)
    text.clear();
  }
  if (!force && text == g.configText) return false;
  g.configText = text;
  std::vector<std::wstring> warnings;
  g.cfg = ParseConfig(text, &warnings);
  Log(L"config loaded (%zu warnings)", warnings.size());
  for (auto& w : warnings) Log(L"  config: %s", w.c_str());
  return true;
}

// -------------------------------------------------------- shared state
bool CreateShared() {
  g.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), kMapName);
  if (!g.mapping) return false;
  g.shared = static_cast<Shared*>(MapViewOfFile(g.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
  if (!g.shared) return false;
  // The TAP keeps the section alive; if a previous ClearBar died mid-write the
  // sequence is odd. Restore even parity (we are the only writer: instance mutex).
  if (ReadAcquire(&g.shared->seq) & 1) InterlockedIncrement(&g.shared->seq);
  g.changed = CreateEventW(nullptr, FALSE, FALSE, kChangedEventName);
  if (!g.changed) return false;
  WriteBegin(g.shared);
  g.shared->magic = kMagic;
  g.shared->version = kProtocolVersion;
  g.shared->exePid = GetCurrentProcessId();
  g.shared->count = 0;
  g.shared->flags = g.cfg.flags;
  g.generation = g.shared->generation + 1;
  g.shared->generation = g.generation;
  WriteEnd(g.shared);
  return true;
}

void Publish(const std::vector<Appearance>& apps, bool force) {
  if (!g.shared) return;
  WriteBegin(g.shared);
  g.shared->magic = kMagic;
  g.shared->version = kProtocolVersion;
  g.shared->exePid = GetCurrentProcessId();
  g.shared->flags = g.cfg.flags;
  if (force) g.shared->generation = ++g.generation;
  uint32_t n = 0;
  for (size_t i = 0; i < g.taskbars.size() && n < static_cast<uint32_t>(kMaxTaskbars); ++i, ++n) {
    g.shared->entries[n].hwnd = reinterpret_cast<uint64_t>(g.taskbars[i].hwnd);
    g.shared->entries[n].app = apps[i];
  }
  g.shared->count = n;
  WriteEnd(g.shared);
  SetEvent(g.changed);
}

void PublishEmpty() {  // tells the TAP to restore every taskbar now
  if (!g.shared) return;
  WriteBegin(g.shared);
  g.shared->count = 0;
  g.shared->exePid = GetCurrentProcessId();
  WriteEnd(g.shared);
  SetEvent(g.changed);
}

// ------------------------------------------------------------ taskbars
uint64_t ProcessCreationTime(DWORD pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return 0;
  FILETIME c{}, e{}, k{}, u{};
  uint64_t t = 0;
  if (GetProcessTimes(p, &c, &e, &k, &u)) t = (static_cast<uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
  CloseHandle(p);
  return t;
}

// The monitor a taskbar belongs to. An auto-hidden bar is slid almost fully
// off its monitor, so prefer points on the bar that are actually on a screen
// (centre first, then edge midpoints) over "largest overlap".
HMONITOR TaskbarMonitor(HWND h) {
  RECT r{};
  if (!GetWindowRect(h, &r)) return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
  const LONG cx = r.left + (r.right - r.left) / 2, cy = r.top + (r.bottom - r.top) / 2;
  const POINT pts[] = {{cx, cy}, {cx, r.top}, {cx, r.bottom - 1}, {r.left, cy}, {r.right - 1, cy}};
  for (const POINT& p : pts)
    if (HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) return m;
  return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
}

void EnumerateTaskbars() {
  g.taskbars.clear();
  HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (!primary) return;
  DWORD pid = 0;
  GetWindowThreadProcessId(primary, &pid);
  if (!pid || !IsRealExplorer(pid)) { Log(L"Shell_TrayWnd not owned by explorer.exe"); return; }
  auto add = [](HWND h) {
    Taskbar t{h, TaskbarMonitor(h), {}};
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(t.mon, &mi)) t.monRect = mi.rcMonitor;
    g.taskbars.push_back(t);
  };
  add(primary);
  struct Ctx { DWORD pid; std::vector<HWND> v; } ctx{pid, {}};
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* c = reinterpret_cast<Ctx*>(lp);
    wchar_t cls[64];
    if (GetClassNameW(h, cls, _countof(cls)) && wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
      DWORD p = 0;
      GetWindowThreadProcessId(h, &p);
      if (p == c->pid) c->v.push_back(h);
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&ctx));
  for (HWND h : ctx.v) add(h);
  for (size_t i = 0; i < g.taskbars.size(); ++i)
    for (size_t j = i + 1; j < g.taskbars.size(); ++j)
      if (g.taskbars[i].mon == g.taskbars[j].mon)
        Log(L"warning: taskbars %p and %p map to the same monitor", g.taskbars[i].hwnd, g.taskbars[j].hwnd);
  g.explorerPid = pid;
  g.explorerCreated = ProcessCreationTime(pid);
  g.lastValid = false;
  Log(L"found %zu taskbar(s), explorer pid %lu", g.taskbars.size(), pid);
}

int TaskbarIndexForMonitor(HMONITOR m) {
  for (size_t i = 0; i < g.taskbars.size(); ++i)
    if (g.taskbars[i].mon == m) return static_cast<int>(i);
  return -1;
}

// ------------------------------------------------------- mode detection
bool IsShellOrSelf(HWND h, const std::wstring& cls) {
  return h == g.wnd || cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" ||
         cls == L"Shell_SecondaryTrayWnd";
}

void GatherFacts(std::vector<MonitorFacts>& facts) {
  facts.assign(g.taskbars.size(), MonitorFacts{});
  if (NeedsShellTracking(g.cfg)) {
    HWND fg = GetForegroundWindow();
    if (fg && !IsCloaked(fg) && IsWindowVisible(fg)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(fg, &pid);
      const std::wstring cls = ClassOf(fg);
      if (cls == L"Windows.UI.Core.CoreWindow" || cls == L"XamlExplorerHostIslandWindow") {
        HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        RECT wr{};
        GetWindowRect(fg, &wr);
        const bool covers = GetMonitorInfoW(mon, &mi) && EqualRect(&wr, &mi.rcMonitor);
        const ShellSurface s = ClassifyForeground(cls, TitleOf(fg), FileNameOf(ProcessImagePath(pid)), covers);
        const int idx = TaskbarIndexForMonitor(mon);
        if (s == ShellSurface::TaskView) {
          for (auto& f : facts) f.taskViewOpen = true;  // Task View spans all monitors
        } else if (idx >= 0 && s == ShellSurface::Start) {
          facts[idx].startOpen = true;
        } else if (idx >= 0 && s == ShellSurface::Search) {
          facts[idx].searchOpen = true;
        }
      }
    }
  }
  g.tracked.clear();
  if (NeedsWindowTracking(g.cfg)) {
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
      auto* facts = reinterpret_cast<std::vector<MonitorFacts>*>(lp);
      if (!IsWindowVisible(h)) return TRUE;  // cheap early out
      const std::wstring cls = ClassOf(h);
      RECT r{};
      GetWindowRect(h, &r);
      WindowFacts w{};
      w.visible = true;
      w.cloaked = IsCloaked(h);
      w.iconic = IsIconic(h) != FALSE;
      w.zoomed = IsZoomed(h) != FALSE;
      w.isRoot = GetAncestor(h, GA_ROOT) == h;
      w.exStyle = static_cast<uint32_t>(GetWindowLongPtrW(h, GWL_EXSTYLE));
      w.emptyRect = IsRectEmpty(&r) != FALSE;
      w.isShellOrSelf = IsShellOrSelf(h, cls);
      w.isCoreWindow = cls == L"Windows.UI.Core.CoreWindow";
      if (!IsUserWindow(w)) return TRUE;
      HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
      g.tracked.push_back({h, mon, w.zoomed});
      const int idx = TaskbarIndexForMonitor(mon);
      if (idx < 0) return TRUE;
      (*facts)[idx].hasVisible = true;
      if (w.zoomed) (*facts)[idx].hasMaximized = true;
      return TRUE;
    }, reinterpret_cast<LPARAM>(&facts));
  }
}

void Recompute(bool force) {
  if (g.taskbars.empty()) return;
  std::vector<MonitorFacts> facts;
  GatherFacts(facts);
  std::vector<Appearance> apps(g.taskbars.size());
  for (size_t i = 0; i < g.taskbars.size(); ++i) {
    const Mode m = ResolveMode(g.cfg, facts[i]);
    apps[i] = g.cfg.modes[m].app;
  }
  if (!force && g.lastValid && apps.size() == g.lastWritten.size() &&
      std::equal(apps.begin(), apps.end(), g.lastWritten.begin()))
    return;
  for (size_t i = 0; i < apps.size(); ++i)
    Log(L"taskbar %p -> kind %u blur %u line %u", g.taskbars[i].hwnd, apps[i].kind, apps[i].blur, apps[i].showLine);
  g.lastWritten = apps;
  g.lastValid = true;
  Publish(apps, force);
}

void ScheduleRecompute() { SetTimer(g.wnd, kTimerRecompute, kRecomputeDelayMs, nullptr); }

// ------------------------------------------------------- win event hooks
const TrackedWindow* FindTracked(HWND h) {
  for (auto& t : g.tracked)
    if (t.hwnd == h) return &t;
  return nullptr;
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
  if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
  switch (event) {
    case EVENT_SYSTEM_FOREGROUND:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_SYSTEM_MOVESIZEEND:
      ScheduleRecompute();
      return;
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_HIDE:
      if (FindTracked(hwnd)) ScheduleRecompute();
      return;
    case EVENT_OBJECT_SHOW:
      if (GetAncestor(hwnd, GA_ROOT) == hwnd) ScheduleRecompute();
      return;
    case EVENT_OBJECT_LOCATIONCHANGE: {
      // Fires a lot; only react when a top-level window's maximised state or
      // monitor changed, or an untracked visible window moved.
      if (GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd)) return;
      const TrackedWindow* t = FindTracked(hwnd);
      const bool zoomed = IsZoomed(hwnd) != FALSE;
      if (!t) { if (zoomed) ScheduleRecompute(); return; }
      if (t->zoomed != zoomed || t->mon != MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL)) ScheduleRecompute();
      return;
    }
  }
}

void AddHook(DWORD min, DWORD max) {
  HWINEVENTHOOK h = SetWinEventHook(min, max, nullptr, WinEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  if (h) g.eventHooks.push_back(h);
  else Log(L"SetWinEventHook %lx-%lx failed %lu", min, max, GetLastError());
}

void UpdateEventHooks() {
  const bool wantShell = NeedsShellTracking(g.cfg) || NeedsWindowTracking(g.cfg);
  const bool wantWindow = NeedsWindowTracking(g.cfg);
  if (wantShell == g.shellHooks && wantWindow == g.windowHooks) return;
  for (auto h : g.eventHooks) UnhookWinEvent(h);
  g.eventHooks.clear();
  if (wantShell) {
    AddHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
    AddHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
  }
  if (wantWindow) {
    AddHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND);
    AddHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    AddHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE);  // DESTROY, SHOW, HIDE
    AddHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE);
  }
  g.shellHooks = wantShell;
  g.windowHooks = wantWindow;
  Log(L"event hooks: shell=%d window=%d (%zu hooks)", wantShell, wantWindow, g.eventHooks.size());
}

// ------------------------------------------------------------ injection
void EndInjectionAttempt() {
  KillTimer(g.wnd, kTimerInjectTimeout);
  if (g.hook) { UnhookWindowsHookEx(g.hook); g.hook = nullptr; }
  if (g.tapModule) { FreeLibrary(g.tapModule); g.tapModule = nullptr; }
}

void CloseReady() {
  EndInjectionAttempt();
  if (g.ready) { CloseHandle(g.ready); g.ready = nullptr; }
  g.tapReady = false;
}

void OnTapReady(bool recompute) {
  EndInjectionAttempt();
  g.tapReady = true;
  g.injectFailures = 0;
  Log(L"TAP ready in explorer %lu", g.explorerPid);
  if (recompute) Recompute(true);
}

void StartInjection() {
  if (g.taskbars.empty() || !g.explorerPid) return;
  if (g.tapReady || g.hook) return;
  if (!g.ready) {
    std::wstring name = std::wstring(kReadyEventPrefix) + std::to_wstring(g.explorerPid);
    g.ready = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (!g.ready) { Log(L"CreateEvent ready failed %lu", GetLastError()); return; }
  }
  // Already there (e.g. ClearBar restarted): callers publish state themselves.
  if (WaitForSingleObject(g.ready, 0) == WAIT_OBJECT_0) { OnTapReady(false); return; }

  // Explorer creates the taskbar windows seconds before their XAML content.
  // Injecting earlier is pointless, so wait (1 s re-checks, only during
  // explorer start-up; TaskbarCreated also brings us back here).
  if (!FindWindowExW(g.taskbars[0].hwnd, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)) {
    if (++g.xamlWaits <= 180) SetTimer(g.wnd, kTimerInjectRetry, 1000, nullptr);
    else Log(L"taskbar XAML never appeared; waiting for TaskbarCreated");
    return;
  }
  g.xamlWaits = 0;

  // Only the module handle + hook address are needed in this process; the
  // hook only ever runs on explorer's taskbar thread, so skip DllMain/imports
  // here to keep this process tiny.
  g.tapModule = LoadLibraryExW(g.tapPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!g.tapModule) { Log(L"cannot load %s (%lu)", g.tapPath.c_str(), GetLastError()); return; }
  auto proc = reinterpret_cast<HOOKPROC>(GetProcAddress(g.tapModule, "ClearBarHookProc"));
  HWND tray = g.taskbars[0].hwnd;
  const DWORD tid = GetWindowThreadProcessId(tray, nullptr);
  if (!proc || !tid) { Log(L"hook proc/tid missing"); EndInjectionAttempt(); return; }
  g.hook = SetWindowsHookExW(WH_GETMESSAGE, proc, g.tapModule, tid);
  if (!g.hook) { Log(L"SetWindowsHookEx failed %lu", GetLastError()); EndInjectionAttempt(); return; }
  PostMessageW(tray, WM_NULL, 0, 0);  // make the taskbar thread pump a message -> loads the DLL
  SetTimer(g.wnd, kTimerInjectTimeout, kInjectTimeoutMs, nullptr);
  Log(L"injecting into explorer %lu thread %lu", g.explorerPid, tid);
}

void OnInjectTimeout() {
  EndInjectionAttempt();
  ++g.injectFailures;
  Log(L"TAP did not become ready (failure %d)", g.injectFailures);
  // An older ClearBarTap generation pinned in explorer blocks ours until explorer restarts.
  const std::wstring legacy = std::wstring(kLegacyReadyEventPrefix) + std::to_wstring(g.explorerPid);
  if (HANDLE old = OpenEventW(SYNCHRONIZE, FALSE, legacy.c_str())) {
    CloseHandle(old);
    Log(L"explorer %lu still has an older ClearBarTap loaded; restart explorer (or sign out) to finish the upgrade",
        g.explorerPid);
  }
  if (g.injectFailures < kMaxInjectFailures)
    SetTimer(g.wnd, kTimerInjectRetry, 3000u * static_cast<UINT>(g.injectFailures), nullptr);
}

// Full reset: re-enumerate taskbars; re-inject if explorer changed.
void ResetState() {
  const DWORD oldPid = g.explorerPid;
  const uint64_t oldCreated = g.explorerCreated;
  EnumerateTaskbars();
  if (g.explorerPid != oldPid || g.explorerCreated != oldCreated || g.taskbars.empty()) {
    CloseReady();
    g.injectFailures = 0;
    g.xamlWaits = 0;
    KillTimer(g.wnd, kTimerInjectRetry);
  }
  if (g.taskbars.empty()) return;  // TaskbarCreated will bring us back
  if (!g.tapReady) StartInjection();
  Recompute(true);
}

// ---------------------------------------------------------- window proc
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g.msgTaskbarCreated && msg) {
    Log(L"TaskbarCreated");
    SetTimer(h, kTimerReset, 250, nullptr);
    return 0;
  }
  if (msg == g.msgTaskbarFound && msg) {
    SetTimer(h, kTimerReset, 100, nullptr);
    return 0;
  }
  switch (msg) {
    case WM_TIMER:
      KillTimer(h, wp);
      switch (wp) {
        case kTimerRecompute: Recompute(false); break;
        case kTimerReset: ResetState(); break;
        case kTimerInjectTimeout: OnInjectTimeout(); break;
        case kTimerInjectRetry:
          StartInjection();
          if (g.tapReady) Recompute(true);
          break;
        case kTimerReload:
          if (LoadConfig(false)) { UpdateEventHooks(); Recompute(true); }
          break;
      }
      return 0;
    case kMsgReload:
      LoadConfig(true);
      UpdateEventHooks();
      ResetState();
      return 0;
    case WM_DISPLAYCHANGE:
      SetTimer(h, kTimerReset, 500, nullptr);
      return 0;
    case WM_SETTINGCHANGE:
      if (wp == SPI_SETWORKAREA) SetTimer(h, kTimerReset, 500, nullptr);
      return 0;
    case WM_POWERBROADCAST:
      if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) SetTimer(h, kTimerReset, 2000, nullptr);
      return TRUE;
    case WM_WTSSESSION_CHANGE:
      if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT) SetTimer(h, kTimerReset, 500, nullptr);
      return 0;
    case WM_ENDSESSION:
      if (wp) DestroyWindow(h);
      return 0;
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

void EnableEfficiencyMode() {
  PROCESS_POWER_THROTTLING_STATE s{};
  s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  s.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
  const std::wstring args = cmd ? cmd : L"";
  if (args.find(L"--exit") != std::wstring::npos || args.find(L"--reload") != std::wstring::npos) {
    HWND w = FindWindowW(kWorkerClass, nullptr);
    if (!w) return 1;
    PostMessageW(w, args.find(L"--exit") != std::wstring::npos ? WM_CLOSE : kMsgReload, 0, 0);
    return 0;
  }

  HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\ClearBar.Instance.v1");
  if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  EnableEfficiencyMode();

  wchar_t exe[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, exe, _countof(exe));
  if (n == 0 || n >= _countof(exe)) return 2;
  g.dir.assign(exe, n);
  g.dir.resize(g.dir.find_last_of(L'\\'));
  g.configPath = g.dir + L"\\config.ini";
  g.logPath = g.dir + L"\\clearbar.log";
  g.tapPath = g.dir + L"\\ClearBarTap.dll";

  WriteDefaultConfigIfMissing();
  LoadConfig(true);
  Log(L"ClearBar starting, pid %lu", GetCurrentProcessId());
  if (!CreateShared()) { Log(L"shared memory setup failed %lu", GetLastError()); return 3; }

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = kWorkerClass;
  if (!RegisterClassExW(&wc)) return 4;
  // Top-level (not message-only) so it receives TaskbarCreated broadcasts.
  g.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWorkerClass, kWorkerClass, WS_POPUP, 0, 0, 0, 0,
                          nullptr, nullptr, inst, nullptr);
  if (!g.wnd) return 5;
  g.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  g.msgTaskbarFound = RegisterWindowMessageW(kTaskbarFoundMsg);
  ChangeWindowMessageFilterEx(g.wnd, g.msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(g.wnd, g.msgTaskbarFound, MSGFLT_ALLOW, nullptr);
  WTSRegisterSessionNotification(g.wnd, NOTIFY_FOR_THIS_SESSION);

  g.dirWatch = FindFirstChangeNotificationW(g.dir.c_str(), FALSE,
                                            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

  UpdateEventHooks();
  ResetState();

  // Event loop: window messages + config-dir change + TAP-ready signal.
  for (;;) {
    HANDLE hs[2];
    DWORD count = 0;
    DWORD dirIdx = MAXDWORD, readyIdx = MAXDWORD;
    if (g.dirWatch != INVALID_HANDLE_VALUE) { dirIdx = count; hs[count++] = g.dirWatch; }
    // Keep listening after an injection timeout: the TAP keeps trying inside
    // explorer and signals whenever it gets there.
    if (g.ready && !g.tapReady) { readyIdx = count; hs[count++] = g.ready; }
    const DWORD r = MsgWaitForMultipleObjectsEx(count, hs, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (r == WAIT_OBJECT_0 + count) {
      MSG m;
      bool quit = false;
      while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { quit = true; break; }
        DispatchMessageW(&m);
      }
      if (quit) break;
    } else if (r < WAIT_OBJECT_0 + count) {
      const DWORD idx = r - WAIT_OBJECT_0;
      if (idx == dirIdx) {
        FindNextChangeNotification(g.dirWatch);
        SetTimer(g.wnd, kTimerReload, 150, nullptr);  // debounce editor save bursts
      } else if (idx == readyIdx) {
        OnTapReady(true);
      }
    } else {
      Sleep(50);  // WAIT_FAILED should not happen; avoid a hot loop if it does
    }
  }

  for (auto h : g.eventHooks) UnhookWinEvent(h);
  EndInjectionAttempt();
  PublishEmpty();  // restore taskbars immediately rather than on process death
  Log(L"ClearBar exiting");
  return 0;
}
