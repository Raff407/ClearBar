// Unit tests for ClearBar's pure logic + the shared-memory seqlock.
#include <windows.h>
#include <stdio.h>
#include <thread>
#include <atomic>
#include "../src/exe/logic.h"

using namespace cb;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (cond) ++g_pass;                                                   \
    else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static void TestBlurValue() {
  uint32_t b = 0;
  CHECK(ParseBlur(L"8", &b) && b == 8);
  CHECK(ParseBlur(L" 1 ", &b) && b == 1);
  CHECK(ParseBlur(L"50", &b) && b == 50);
  CHECK(!ParseBlur(L"0", &b));
  CHECK(!ParseBlur(L"51", &b));
  CHECK(!ParseBlur(L"-3", &b));
  CHECK(!ParseBlur(L"8.5", &b));
  CHECK(!ParseBlur(L"", &b));
  CHECK(!ParseBlur(L"9999", &b));
}

static void TestBoolAccent() {
  bool b;
  CHECK(ParseBool(L"TRUE", &b) && b);
  CHECK(ParseBool(L" off ", &b) && !b);
  CHECK(!ParseBool(L"maybe", &b));
  Kind k;
  CHECK(ParseAccent(L"Clear", &k) && k == Kind::Clear);
  CHECK(ParseAccent(L"blur", &k) && k == Kind::Blur);
  CHECK(ParseAccent(L"normal", &k) && k == Kind::Normal);
  CHECK(!ParseAccent(L"opaque", &k));   // colours removed
  CHECK(!ParseAccent(L"acrylic", &k));
}

static void TestDefaults() {
  Config c = ParseConfig(L"", nullptr);
  CHECK(c.modes[ModeDesktop].enabled);
  CHECK(c.modes[ModeDesktop].app.kind == (uint32_t)Kind::Clear);
  CHECK(c.modes[ModeDesktop].app.showLine == 0);
  CHECK(c.modes[ModeDesktop].app.blur == kDefaultBlur);
  CHECK(c.modes[ModeTaskViewOpened].enabled);
  CHECK(c.modes[ModeTaskViewOpened].app.kind == (uint32_t)Kind::Normal);
  CHECK(c.modes[ModeTaskViewOpened].app.showLine == 1);
  CHECK(!c.modes[ModeMaximizedWindow].enabled);
  CHECK(c.modes[ModeMaximizedWindow].app.kind == (uint32_t)Kind::Blur);
  CHECK(c.flags == 0);
}

static void TestParseFull() {
  std::vector<std::wstring> w;
  Config c = ParseConfig(
      L"\xFEFF; comment\r\n"
      L"# also comment\r\n"
      L"[General]\r\n"
      L"debug_log = yes\r\n"
      L"[desktop]\r\n"
      L"accent = blur\r\n"
      L"blur = 12   # soft\r\n"
      L"show_line = true ; inline\r\n"
      L"enabled = false\r\n"         // ignored + warned
      L"color = #FF0000\r\n"         // removed feature: warned, ignored
      L"[maximized_window]\r\n"
      L"enabled=true\r\n"
      L"accent=clear\r\n"
      L"[nonsense]\r\n"
      L"foo = bar\r\n"
      L"[visible_window]\r\n"
      L"accent = opaque\r\n"
      L"blur = 99\r\n"
      L"garbage line\r\n"
      L"[task_view_opened\r\n",
      &w);
  CHECK(c.flags & kFlagDebugLog);
  CHECK(c.modes[ModeDesktop].enabled);
  CHECK(c.modes[ModeDesktop].app.kind == (uint32_t)Kind::Blur);
  CHECK(c.modes[ModeDesktop].app.blur == 12);
  CHECK(c.modes[ModeDesktop].app.showLine == 1);
  CHECK(c.modes[ModeMaximizedWindow].enabled);
  CHECK(c.modes[ModeMaximizedWindow].app.kind == (uint32_t)Kind::Clear);
  // invalid values keep defaults
  CHECK(c.modes[ModeVisibleWindow].app.kind == (uint32_t)Kind::Clear);
  CHECK(c.modes[ModeVisibleWindow].app.blur == kDefaultBlur);
  // warnings: desktop disable, color, unknown section, foo, opaque, blur 99, garbage, malformed header
  CHECK(w.size() == 8);
}

static void TestCommentStripping() {
  CHECK(StripComment(L"blur = 8") == L"blur = 8");
  CHECK(StripComment(L"blur = 8 # c") == L"blur = 8");
  CHECK(StripComment(L"blur =#8") == L"blur =#8");
  CHECK(StripComment(L"  ; whole") == L"");
  CHECK(StripComment(L"#whole") == L"");
  CHECK(StripComment(L"a = b;c") == L"a = b");
  CHECK(StripComment(L"[sec] ; x") == L"[sec]");
}

static void TestLfOnlyAndNoTrailingNewline() {
  Config c = ParseConfig(L"[desktop]\naccent=normal\nshow_line=true", nullptr);
  CHECK(c.modes[ModeDesktop].app.kind == (uint32_t)Kind::Normal);
  CHECK(c.modes[ModeDesktop].app.showLine == 1);
}

static void TestResolve() {
  Config c = DefaultConfig();
  MonitorFacts f;
  CHECK(ResolveMode(c, f) == ModeDesktop);
  f.hasMaximized = true;
  CHECK(ResolveMode(c, f) == ModeDesktop);  // disabled by default
  c.modes[ModeVisibleWindow].enabled = true;
  CHECK(ResolveMode(c, f) == ModeVisibleWindow);  // maximised counts as visible
  c.modes[ModeMaximizedWindow].enabled = true;
  CHECK(ResolveMode(c, f) == ModeMaximizedWindow);
  f.searchOpen = true;
  CHECK(ResolveMode(c, f) == ModeMaximizedWindow);  // search disabled
  c.modes[ModeSearchOpened].enabled = true;
  CHECK(ResolveMode(c, f) == ModeSearchOpened);
  f.startOpen = true;
  c.modes[ModeStartOpened].enabled = true;
  CHECK(ResolveMode(c, f) == ModeStartOpened);
  f.taskViewOpen = true;
  CHECK(ResolveMode(c, f) == ModeTaskViewOpened);
  c.modes[ModeTaskViewOpened].enabled = false;
  CHECK(ResolveMode(c, f) == ModeStartOpened);
  CHECK(NeedsWindowTracking(c));
  CHECK(NeedsShellTracking(DefaultConfig()));
  CHECK(!NeedsWindowTracking(DefaultConfig()));
}

static void TestClassify() {
  CHECK(ClassifyForeground(L"Windows.UI.Core.CoreWindow", L"Search", L"SearchHost.exe", false) == ShellSurface::Search);
  CHECK(ClassifyForeground(L"Windows.UI.Core.CoreWindow", L"Start", L"StartMenuExperienceHost.exe", false) == ShellSurface::Start);
  CHECK(ClassifyForeground(L"Windows.UI.Core.CoreWindow", L"x", L"ApplicationFrameHost.exe", true) == ShellSurface::None);
  CHECK(ClassifyForeground(L"XamlExplorerHostIslandWindow", L"Task View", L"explorer.exe", true) == ShellSurface::TaskView);
  CHECK(ClassifyForeground(L"XamlExplorerHostIslandWindow", L"Vue des tâches", L"EXPLORER.EXE", true) == ShellSurface::TaskView);
  CHECK(ClassifyForeground(L"XamlExplorerHostIslandWindow", L"Task Switching", L"explorer.exe", true) == ShellSurface::None);
  CHECK(ClassifyForeground(L"XamlExplorerHostIslandWindow", L"Task View", L"evil.exe", true) == ShellSurface::None);
  CHECK(ClassifyForeground(L"Chrome_WidgetWin_1", L"Task View", L"explorer.exe", true) == ShellSurface::None);
}

static void TestUserWindow() {
  WindowFacts w{true, false, false, false, true, 0, false, false, false};
  CHECK(IsUserWindow(w));
  WindowFacts t = w; t.exStyle = 0x80; CHECK(!IsUserWindow(t));           // tool window
  t.exStyle = 0x80 | 0x40000; CHECK(IsUserWindow(t));                      // tool + appwindow
  t = w; t.exStyle = 0x08000000; CHECK(!IsUserWindow(t));                  // noactivate
  t = w; t.cloaked = true; CHECK(!IsUserWindow(t));
  t = w; t.iconic = true; CHECK(!IsUserWindow(t));
  t = w; t.isShellOrSelf = true; CHECK(!IsUserWindow(t));
  t = w; t.isCoreWindow = true; CHECK(!IsUserWindow(t));
  t = w; t.emptyRect = true; CHECK(!IsUserWindow(t));
  t = w; t.visible = false; CHECK(!IsUserWindow(t));
}

static void TestProtocol() {
  Shared s{};
  s.magic = kMagic; s.version = kProtocolVersion; s.count = 2;
  s.entries[0] = {0x1234, MakeApp(Kind::Clear, false)};
  s.entries[1] = {0x5678, MakeApp(Kind::Blur, true, 20)};
  CHECK(Validate(s));
  CHECK(FindEntry(s, 0x5678) && FindEntry(s, 0x5678)->kind == (uint32_t)Kind::Blur && FindEntry(s, 0x5678)->blur == 20);
  CHECK(!FindEntry(s, 0x9999));
  Shared bad = s; bad.count = kMaxTaskbars + 1; CHECK(!Validate(bad));
  bad = s; bad.entries[1].app.kind = 7; CHECK(!Validate(bad));
  bad = s; bad.version = 99; CHECK(!Validate(bad));
  bad = s; bad.entries[0].app.showLine = 2; CHECK(!Validate(bad));
  bad = s; bad.entries[1].app.blur = kMaxBlur + 1; CHECK(!Validate(bad));
  CHECK(MakeApp(Kind::Blur, true, 5) != MakeApp(Kind::Blur, true, 6));
  CHECK(sizeof(Shared) == 32 + kMaxTaskbars * 24);
}

// Writer hammers consistent records; readers must never see a torn one.
static void TestSeqlockStress() {
  static Shared s{};
  s.magic = kMagic; s.version = kProtocolVersion;
  std::atomic<bool> stop{false};
  std::atomic<long> torn{0}, reads{0};
  std::thread writer([&] {
    for (uint32_t i = 1; i < 400000; ++i) {
      WriteBegin(&s);
      s.count = 1 + (i % kMaxTaskbars);
      for (uint32_t k = 0; k < s.count; ++k) s.entries[k] = {i, {i % 3, i, i & 1, 0}};  // blur field reused as a stamp
      s.generation = i;
      WriteEnd(&s);
    }
    stop = true;
  });
  auto reader = [&] {
    while (!stop) {
      Shared snap;
      if (!ReadSnapshot(&s, &snap)) continue;
      ++reads;
      for (uint32_t k = 0; k < snap.count; ++k)
        if (snap.entries[k].hwnd != snap.generation || snap.entries[k].app.blur != snap.generation) { ++torn; break; }
    }
  };
  std::thread r1(reader), r2(reader);
  writer.join(); r1.join(); r2.join();
  printf("  seqlock: %ld consistent reads, %ld torn\n", reads.load(), torn.load());
  CHECK(torn == 0);
  CHECK(reads > 0);
}

int main() {
  TestBlurValue();
  TestBoolAccent();
  TestDefaults();
  TestParseFull();
  TestCommentStripping();
  TestLfOnlyAndNoTrailingNewline();
  TestResolve();
  TestClassify();
  TestUserWindow();
  TestProtocol();
  TestSeqlockStress();
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
