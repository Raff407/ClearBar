// Pure logic for ClearBar.exe: config parsing, mode resolution and window
// classification. No Win32 side effects here so it can be unit tested.
#pragma once
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>
#include <cwctype>
#include "../common/protocol.h"

namespace cb {

enum Mode : int {
  ModeDesktop = 0,
  ModeVisibleWindow,
  ModeMaximizedWindow,
  ModeStartOpened,
  ModeSearchOpened,
  ModeTaskViewOpened,
  ModeCount
};

inline constexpr const wchar_t* kModeSection[ModeCount] = {
    L"desktop",        L"visible_window", L"maximized_window",
    L"start_opened",   L"search_opened",  L"task_view_opened"};

struct ModeConfig {
  bool enabled;
  Appearance app;
};

struct Config {
  ModeConfig modes[ModeCount];
  uint32_t flags;  // kFlagDebugLog | kFlagTraceTree
};

inline Appearance MakeApp(Kind k, bool line, uint32_t blur = kDefaultBlur) {
  return Appearance{static_cast<uint32_t>(k), blur, line ? 1u : 0u, 0u};
}

// Defaults: always clear with no line; Windows' normal taskbar (with its line)
// while Task View is open.
inline Config DefaultConfig() {
  Config c{};
  c.modes[ModeDesktop] = {true, MakeApp(Kind::Clear, false)};
  c.modes[ModeVisibleWindow] = {false, MakeApp(Kind::Clear, false)};
  c.modes[ModeMaximizedWindow] = {false, MakeApp(Kind::Blur, true)};
  c.modes[ModeStartOpened] = {false, MakeApp(Kind::Normal, true)};
  c.modes[ModeSearchOpened] = {false, MakeApp(Kind::Normal, true)};
  c.modes[ModeTaskViewOpened] = {true, MakeApp(Kind::Normal, true)};
  c.flags = 0;
  return c;
}

// ---------------------------------------------------------------- parsing

inline std::wstring_view Trim(std::wstring_view s) {
  size_t b = 0, e = s.size();
  while (b < e && iswspace(s[b])) ++b;
  while (e > b && iswspace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

inline std::wstring Lower(std::wstring_view s) {
  std::wstring r(s);
  for (auto& ch : r) ch = static_cast<wchar_t>(towlower(ch));
  return r;
}

inline bool ParseBool(std::wstring_view v, bool* out) {
  const std::wstring l = Lower(Trim(v));
  if (l == L"true" || l == L"1" || l == L"yes" || l == L"on") { *out = true; return true; }
  if (l == L"false" || l == L"0" || l == L"no" || l == L"off") { *out = false; return true; }
  return false;
}

// accent: clear | blur | normal  (returns false if unknown)
//   clear  = fully see-through
//   blur   = see-through with a Gaussian blur of whatever is behind the bar
//   normal = Windows' own taskbar background
inline bool ParseAccent(std::wstring_view v, Kind* out) {
  const std::wstring l = Lower(Trim(v));
  if (l == L"normal") { *out = Kind::Normal; return true; }
  if (l == L"clear") { *out = Kind::Clear; return true; }
  if (l == L"blur") { *out = Kind::Blur; return true; }
  return false;
}

// Whole number kMinBlur..kMaxBlur.
inline bool ParseBlur(std::wstring_view v, uint32_t* out) {
  v = Trim(v);
  if (v.empty() || v.size() > 3) return false;
  uint32_t x = 0;
  for (wchar_t c : v) {
    if (c < L'0' || c > L'9') return false;
    x = x * 10 + static_cast<uint32_t>(c - L'0');
  }
  if (x < kMinBlur || x > kMaxBlur) return false;
  *out = x;
  return true;
}

// Comments: a line starting with ';' or '#'; ';' anywhere; or ' #' after a
// complete "key = value" (e.g. "blur = 8  # soft").
inline std::wstring_view StripComment(std::wstring_view line) {
  line = Trim(line);
  if (line.empty() || line[0] == L';' || line[0] == L'#') return {};
  const size_t semi = line.find(L';');
  if (semi != std::wstring_view::npos) line = Trim(line.substr(0, semi));
  const size_t eq = line.find(L'=');
  if (eq != std::wstring_view::npos) {
    for (size_t i = eq + 1; i < line.size(); ++i) {
      if (line[i] == L'#' && iswspace(line[i - 1]) && !Trim(line.substr(eq + 1, i - eq - 1)).empty()) {
        line = Trim(line.substr(0, i));
        break;
      }
    }
  }
  return line;
}

// Parses INI text. Unknown sections/keys and bad values are reported in
// `warnings` and otherwise ignored (the default stays in place). Never fails.
inline Config ParseConfig(std::wstring_view text, std::vector<std::wstring>* warnings) {
  Config c = DefaultConfig();
  int section = -2;  // -2 none, -1 [general], >=0 mode index
  size_t lineNo = 0;
  auto warn = [&](const std::wstring& w) {
    if (warnings) warnings->push_back(L"line " + std::to_wstring(lineNo) + L": " + w);
  };
  // Strip UTF-8/16 BOM if it survived decoding.
  if (!text.empty() && text[0] == 0xFEFF) text.remove_prefix(1);

  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find(L'\n', pos);
    if (nl == std::wstring_view::npos) nl = text.size();
    std::wstring_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    ++lineNo;
    line = StripComment(line);
    if (line.empty()) continue;
    if (line.front() == L'[') {
      if (line.back() != L']') { warn(L"malformed section header"); section = -2; continue; }
      const std::wstring name = Lower(Trim(line.substr(1, line.size() - 2)));
      section = -2;
      if (name == L"general") section = -1;
      for (int m = 0; m < ModeCount; ++m)
        if (name == kModeSection[m]) section = m;
      if (section == -2) warn(L"unknown section [" + name + L"]");
      continue;
    }
    const size_t eq = line.find(L'=');
    if (eq == std::wstring_view::npos) { warn(L"expected key = value"); continue; }
    const std::wstring key = Lower(Trim(line.substr(0, eq)));
    const std::wstring_view val = Trim(line.substr(eq + 1));
    if (section == -2) { warn(L"key outside a known section: " + key); continue; }
    if (section == -1) {
      bool b;
      if (key == L"debug_log") {
        if (ParseBool(val, &b)) c.flags = b ? (c.flags | kFlagDebugLog) : (c.flags & ~kFlagDebugLog);
        else warn(L"bad bool for debug_log");
      } else if (key == L"trace_tree") {
        if (ParseBool(val, &b)) c.flags = b ? (c.flags | kFlagTraceTree) : (c.flags & ~kFlagTraceTree);
        else warn(L"bad bool for trace_tree");
      } else {
        warn(L"unknown key " + key);
      }
      continue;
    }
    ModeConfig& mc = c.modes[section];
    if (key == L"enabled") {
      bool b;
      if (!ParseBool(val, &b)) warn(L"bad bool for enabled");
      else if (section == ModeDesktop && !b) warn(L"desktop mode cannot be disabled");
      else mc.enabled = b;
    } else if (key == L"accent") {
      Kind k;
      if (!ParseAccent(val, &k)) warn(L"bad accent (clear|blur|normal)");
      else mc.app.kind = static_cast<uint32_t>(k);
    } else if (key == L"blur") {
      uint32_t b;
      if (!ParseBlur(val, &b)) warn(L"bad blur (whole number 1-50)");
      else mc.app.blur = b;
    } else if (key == L"show_line") {
      bool b;
      if (!ParseBool(val, &b)) warn(L"bad bool for show_line");
      else mc.app.showLine = b ? 1u : 0u;
    } else if (key == L"color" || key == L"colour") {
      warn(L"colours were removed; ignoring " + key);
    } else {
      warn(L"unknown key " + key);
    }
  }
  c.modes[ModeDesktop].enabled = true;
  return c;
}

// ------------------------------------------------------------ resolution

struct MonitorFacts {
  bool hasVisible = false;
  bool hasMaximized = false;
  bool startOpen = false;
  bool searchOpen = false;
  bool taskViewOpen = false;
};

// Priority: Task View > Start > Search > maximised > visible > desktop.
inline Mode ResolveMode(const Config& c, const MonitorFacts& f) {
  if (f.taskViewOpen && c.modes[ModeTaskViewOpened].enabled) return ModeTaskViewOpened;
  if (f.startOpen && c.modes[ModeStartOpened].enabled) return ModeStartOpened;
  if (f.searchOpen && c.modes[ModeSearchOpened].enabled) return ModeSearchOpened;
  if (f.hasMaximized && c.modes[ModeMaximizedWindow].enabled) return ModeMaximizedWindow;
  if ((f.hasVisible || f.hasMaximized) && c.modes[ModeVisibleWindow].enabled) return ModeVisibleWindow;
  return ModeDesktop;
}

inline bool NeedsWindowTracking(const Config& c) {
  return c.modes[ModeVisibleWindow].enabled || c.modes[ModeMaximizedWindow].enabled;
}
inline bool NeedsShellTracking(const Config& c) {
  return c.modes[ModeStartOpened].enabled || c.modes[ModeSearchOpened].enabled ||
         c.modes[ModeTaskViewOpened].enabled;
}

// ---------------------------------------------------- window classification

enum class ShellSurface { None, Start, Search, TaskView };

inline bool IEquals(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (towlower(a[i]) != towlower(b[i])) return false;
  return true;
}

// Classifies the foreground window. `exeName` is the owning process image
// file name (no path). `coversMonitor` = window rect equals its monitor rect.
inline ShellSurface ClassifyForeground(std::wstring_view cls, std::wstring_view title,
                                       std::wstring_view exeName, bool coversMonitor) {
  if (cls == L"Windows.UI.Core.CoreWindow") {
    if (IEquals(exeName, L"StartMenuExperienceHost.exe")) return ShellSurface::Start;
    if (IEquals(exeName, L"SearchHost.exe") || IEquals(exeName, L"SearchApp.exe"))
      return ShellSurface::Search;
    return ShellSurface::None;
  }
  if (cls == L"XamlExplorerHostIslandWindow" && IEquals(exeName, L"explorer.exe")) {
    if (title == L"Task Switching") return ShellSurface::None;  // Alt+Tab
    if (title == L"Task View" || coversMonitor) return ShellSurface::TaskView;
  }
  return ShellSurface::None;
}

// Window-filter facts for "is this a real user window" (pure part).
struct WindowFacts {
  bool visible, cloaked, iconic, zoomed, isRoot;
  uint32_t exStyle;
  bool emptyRect;
  bool isShellOrSelf;  // taskbar/desktop/our window
  bool isCoreWindow;
};
inline bool IsUserWindow(const WindowFacts& w) {
  constexpr uint32_t kToolWindow = 0x00000080L, kAppWindow = 0x00040000L,
                     kNoActivate = 0x08000000L;
  if (!w.visible || w.cloaked || w.iconic || !w.isRoot || w.emptyRect) return false;
  if (w.isShellOrSelf || w.isCoreWindow) return false;
  const bool appWindow = (w.exStyle & kAppWindow) != 0;
  if ((w.exStyle & kToolWindow) && !appWindow) return false;
  if ((w.exStyle & kNoActivate) && !appWindow) return false;
  return true;
}

}  // namespace cb
