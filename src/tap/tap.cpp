// ClearBarTap.dll - runs inside explorer.exe.
//
// Loaded on the taskbar UI thread via a no-op WH_GETMESSAGE hook set by
// ClearBar.exe. It then registers itself with the documented XAML Diagnostics
// API (InitializeXamlDiagnosticsEx, the mechanism Visual Studio's Live Visual
// Tree uses), watches the taskbar's visual tree, and sets the Fill of the
// taskbar's `BackgroundFill` / `BackgroundStroke` rectangles according to the
// appearance ClearBar.exe publishes in shared memory.
//
// Safety rules followed here:
//  * no exceptions escape any callback; every winrt call is guarded
//  * XAML objects are only touched on their own UI thread (DispatcherQueue)
//  * original values are restored when ClearBar.exe exits or dies
//  * explorer fighting back is rate-limited (no ping-pong CPU burn)
//  * no network, no files except an opt-in debug log next to the DLL
#include <windows.h>
#include <unknwn.h>
#include <ocidl.h>
#include <oleauto.h>
#include <psapi.h>
#include <xamlOM.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#undef GetCurrentTime  // winbase macro collides with Storyboard::GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdarg.h>

#include "../common/protocol.h"

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dxguid.lib")

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxs = winrt::Windows::UI::Xaml::Shapes;
namespace ws = winrt::Windows::System;
namespace wuc = winrt::Windows::UI::Composition;
namespace wge = winrt::Windows::Graphics::Effects;
namespace awge = ABI::Windows::Graphics::Effects;
using namespace cb;

// {6E1F2C3B-8A4D-4F7E-9B21-3C5D7E9F0A42}
static constexpr GUID CLSID_ClearBarTap = {0x6e1f2c3b, 0x8a4d, 0x4f7e, {0x9b, 0x21, 0x3c, 0x5d, 0x7e, 0x9f, 0x0a, 0x42}};

namespace {

HMODULE g_module = nullptr;
std::atomic<uint32_t> g_flags{0};

struct SrwGuard {
  SRWLOCK* l;
  explicit SrwGuard(SRWLOCK* x) : l(x) { AcquireSRWLockExclusive(l); }
  ~SrwGuard() { ReleaseSRWLockExclusive(l); }
};

// ------------------------------------------------------------------ log
SRWLOCK g_logLock = SRWLOCK_INIT;
wchar_t g_logPath[MAX_PATH * 2];

void Log(const wchar_t* fmt, ...) {
  if (!(g_flags.load(std::memory_order_relaxed) & kFlagDebugLog) || !g_logPath[0]) return;
  wchar_t buf[1024];
  SYSTEMTIME st;
  GetLocalTime(&st);
  int n = swprintf_s(buf, L"%02d:%02d:%02d.%03d [%5lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     GetCurrentThreadId());
  if (n < 0) n = 0;
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf + n, _countof(buf) - n - 3, _TRUNCATE, fmt, ap);
  va_end(ap);
  wcscat_s(buf, L"\r\n");
  char utf8[3072];
  const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
  if (len <= 1) return;
  SrwGuard g(&g_logLock);
  HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(f, &size) && size.QuadPart > 4 * 1024 * 1024) {
    CloseHandle(f);
    f = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
  }
  DWORD w;
  WriteFile(f, utf8, static_cast<DWORD>(len - 1), &w, nullptr);
  CloseHandle(f);
}

// --------------------------------------------- DisableCompositionDiag shim
// XAML's composition diagnostics rebuild a process-wide visual walker without
// locking whenever a composition visual is added; with several XAML threads
// (explorer has many) that can corrupt the heap. We only need element
// add/remove notifications, so make Windows.UI.Xaml.dll believe
// HKLM\Software\Microsoft\XAML\Debug\DisableCompositionDiag = 1 - but only on
// our advise thread, only for that value. Nothing is written to the registry.
thread_local bool t_fakeDiagDisabled = false;

using RegOpenKeyExW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using RegQueryValueExW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegGetValueW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
RegOpenKeyExW_t g_origOpen = nullptr;
RegQueryValueExW_t g_origQuery = nullptr;
RegGetValueW_t g_origGet = nullptr;

LSTATUS WINAPI HookRegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
  LSTATUS r = g_origOpen(key, sub, opt, sam, out);
  if (r == ERROR_SUCCESS || !t_fakeDiagDisabled || key != HKEY_LOCAL_MACHINE || !sub ||
      _wcsicmp(sub, L"Software\\Microsoft\\XAML\\Debug") != 0)
    return r;
  // Key normally doesn't exist; hand out one that does so the value is queried.
  return g_origOpen(HKEY_LOCAL_MACHINE, L"Software\\Microsoft", opt, sam, out);
}

LSTATUS FakeDword(LPDWORD type, LPBYTE data, LPDWORD cb) {
  if (type) *type = REG_DWORD;
  if (data && (!cb || *cb < sizeof(DWORD))) {
    if (cb) *cb = sizeof(DWORD);
    return ERROR_MORE_DATA;
  }
  if (data) *reinterpret_cast<DWORD*>(data) = 1;
  if (cb) *cb = sizeof(DWORD);
  return ERROR_SUCCESS;
}

LSTATUS WINAPI HookRegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb) {
  if (!t_fakeDiagDisabled || !name || _wcsicmp(name, L"DisableCompositionDiag") != 0)
    return g_origQuery(key, name, res, type, data, cb);
  Log(L"reporting DisableCompositionDiag=1 (RegQueryValueExW)");
  return FakeDword(type, data, cb);
}

LSTATUS WINAPI HookRegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data,
                                LPDWORD cb) {
  if (!t_fakeDiagDisabled || !name || _wcsicmp(name, L"DisableCompositionDiag") != 0)
    return g_origGet(key, sub, name, flags, type, data, cb);
  Log(L"reporting DisableCompositionDiag=1 (RegGetValueW)");
  return FakeDword(type, static_cast<LPBYTE>(data), cb);
}

// Patches by-name imports of `mod` whose name equals `func`. Returns count.
int PatchImport(HMODULE mod, const char* func, void* hook, void** orig) {
  auto base = reinterpret_cast<BYTE*>(mod);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress || !dir.Size) return 0;
  int patched = 0;
  for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++addrs) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(ibn->Name), func) != 0) continue;
      DWORD old;
      if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
      if (!*orig) *orig = reinterpret_cast<void*>(addrs->u1.Function);
      InterlockedExchangePointer(reinterpret_cast<PVOID*>(&addrs->u1.Function), hook);
      VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
      ++patched;
    }
  }
  return patched;
}

void InstallRegistryShim(HMODULE wux) {
  // Once only: a second pass would read our own hooks back as "originals".
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  // Resolve originals first so a hook can never call through a null pointer.
  HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
  if (!kb) return;
  auto open = reinterpret_cast<RegOpenKeyExW_t>(GetProcAddress(kb, "RegOpenKeyExW"));
  auto query = reinterpret_cast<RegQueryValueExW_t>(GetProcAddress(kb, "RegQueryValueExW"));
  auto get = reinterpret_cast<RegGetValueW_t>(GetProcAddress(kb, "RegGetValueW"));
  if (!open || !query || !get) return;
  // Patched IAT slots keep pointing at these hooks (pinned DLL), which pass
  // straight through unless t_fakeDiagDisabled is set on the calling thread.
  void* o = nullptr;
  void* q = nullptr;
  void* gv = nullptr;
  g_origOpen = open;
  g_origQuery = query;
  g_origGet = get;
  const int a = PatchImport(wux, "RegOpenKeyExW", reinterpret_cast<void*>(&HookRegOpenKeyExW), &o);
  const int b = PatchImport(wux, "RegQueryValueExW", reinterpret_cast<void*>(&HookRegQueryValueExW), &q);
  const int c = PatchImport(wux, "RegGetValueW", reinterpret_cast<void*>(&HookRegGetValueW), &gv);
  // Prefer the module's own previous targets (may be forwarders/other shims).
  if (o) g_origOpen = reinterpret_cast<RegOpenKeyExW_t>(o);
  if (q) g_origQuery = reinterpret_cast<RegQueryValueExW_t>(q);
  if (gv) g_origGet = reinterpret_cast<RegGetValueW_t>(gv);
  Log(L"registry shim: patched %d/%d/%d import slots", a, b, c);
}

// --------------------------------------------------------- shared state
SRWLOCK g_lock = SRWLOCK_INIT;  // guards everything in this block
Shared g_snap{};
bool g_haveSnap = false;
bool g_exeAlive = false;
std::vector<InstanceHandle> g_sources;
struct Rec;
// XAML/COM holders are intentionally leaked heap objects: no destructor may run
// at explorer exit (DLL_PROCESS_DETACH under loader lock, UI threads gone).
std::vector<std::shared_ptr<Rec>>& g_recs = *new std::vector<std::shared_ptr<Rec>>();

winrt::com_ptr<IXamlDiagnostics>& g_diag = *new winrt::com_ptr<IXamlDiagnostics>();  // guarded by g_lock

// Readers always work on a local copy; never dereference g_diag directly.
winrt::com_ptr<IXamlDiagnostics> Diag() {
  SrwGuard g(&g_lock);
  return g_diag;
}
std::atomic<bool> g_siteSet{false};
HANDLE g_readyEvent = nullptr;

// Returns false => restore Windows' default for this taskbar.
bool LookupDesired(HWND taskbar, Appearance* out) {
  SrwGuard g(&g_lock);
  if (!taskbar || !g_haveSnap || !g_exeAlive) return false;
  const Appearance* a = FindEntry(g_snap, reinterpret_cast<uint64_t>(taskbar));
  if (!a) return false;
  *out = *a;
  return true;
}

bool HasEntry(HWND taskbar) {
  SrwGuard g(&g_lock);
  return g_haveSnap && g_exeAlive && FindEntry(g_snap, reinterpret_cast<uint64_t>(taskbar)) != nullptr;
}

// ------------------------------------------------------- per-taskbar data
struct Part {
  wuxs::Rectangle rect{nullptr};
  InstanceHandle handle = 0;
  int64_t token = 0;
  bool hasToken = false;
  bool modified = false;                  // our brush is currently set
  wf::IInspectable origLocal{nullptr};    // local value before we touched it
  wuxm::Brush ours{nullptr};
  uint64_t oursKey = ~0ull;               // identity of the brush we built
  bool applying = false;
  // explorer-fights-back limiter
  ULONGLONG burstStart = 0;
  int burstCount = 0;
  bool suspended = false;
};

struct Rec {
  wux::FrameworkElement frame{nullptr};
  InstanceHandle frameHandle = 0;
  ws::DispatcherQueue queue{nullptr};  // immutable after creation
  HWND hwnd = nullptr;                 // Shell_TrayWnd / Shell_SecondaryTrayWnd
  bool notifiedExe = false;
  Part fill, stroke;
};

bool IsTaskbarWindow(HWND h) {
  wchar_t cls[64];
  if (!h || !GetClassNameW(h, cls, _countof(cls))) return false;
  return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

// Must run on rec's UI thread.
void ResolveHwnd(Rec& rec) {
  if (rec.hwnd && IsWindow(rec.hwnd)) return;
  rec.hwnd = nullptr;
  const auto diag = Diag();
  if (!diag || !rec.frame) return;
  wux::XamlRoot root{nullptr};
  wux::UIElement content{nullptr};
  try {
    root = rec.frame.XamlRoot();
    if (root) content = root.Content();
  } catch (...) {}
  if (!root) return;
  std::vector<InstanceHandle> sources;
  {
    SrwGuard g(&g_lock);
    sources = g_sources;
  }
  for (InstanceHandle h : sources) {
    try {
      wf::IInspectable insp;
      if (FAILED(diag->GetIInspectableFromHandle(h, reinterpret_cast<::IInspectable**>(winrt::put_abi(insp)))))
        continue;
      auto src = insp.try_as<wux::Hosting::DesktopWindowXamlSource>();
      if (!src) continue;
      auto c = src.Content();  // throws wrong_thread for islands on other threads
      if (!c) continue;
      bool match = content && c == content;
      if (!match) {
        auto cr = c.XamlRoot();
        match = cr && cr == root;
      }
      if (!match) continue;
      HWND island = nullptr;
      if (FAILED(src.as<IDesktopWindowXamlSourceNative>()->get_WindowHandle(&island)) || !island) continue;
      HWND parent = GetAncestor(island, GA_PARENT);
      if (IsTaskbarWindow(parent)) {
        rec.hwnd = parent;
        Log(L"frame %llx -> taskbar %p (island %p)", rec.frameHandle, parent, island);
        return;
      }
    } catch (...) {
    }
  }
  Log(L"frame %llx: no matching XAML island yet (%zu sources)", rec.frameHandle, sources.size());
}

// Sets (brush != null) or restores (brush == null) one rectangle's Fill.
void SetPart(Part& p, const wuxm::Brush& brush, uint64_t key) {
  if (!p.rect) return;
  const auto fillProp = wuxs::Shape::FillProperty();
  if (!brush) {
    if (!p.modified) return;
    p.applying = true;
    try {
      if (!p.origLocal || p.origLocal == wux::DependencyProperty::UnsetValue())
        p.rect.ClearValue(fillProp);
      else
        p.rect.SetValue(fillProp, p.origLocal);
    } catch (...) {
    }
    p.applying = false;
    p.modified = false;
    p.ours = nullptr;
    p.oursKey = ~0ull;
    p.origLocal = nullptr;
    return;
  }
  p.applying = true;
  try {
    if (!p.modified) p.origLocal = p.rect.ReadLocalValue(fillProp);
    p.rect.SetValue(fillProp, brush);
    p.modified = true;
    p.ours = brush;
    p.oursKey = key;
  } catch (...) {
  }
  p.applying = false;
}

// ------------------------------------------------------------ blur brush
// A XAML brush whose composition brush is "backdrop -> D2D Gaussian blur".
// Unlike AcrylicBrush this does not fall back to a flat colour when Windows
// "Transparency effects" is off, and it has no tint: pure blur of whatever
// is behind the taskbar.
struct GaussianBlurEffect
    : winrt::implements<GaussianBlurEffect, wge::IGraphicsEffect, wge::IGraphicsEffectSource,
                        awge::IGraphicsEffectD2D1Interop> {
  wge::IGraphicsEffectSource source{nullptr};
  float amount = static_cast<float>(kDefaultBlur);
  winrt::hstring name{L"ClearBarBlur"};

  winrt::hstring Name() { return name; }
  void Name(winrt::hstring const& n) { name = n; }

  HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override {
    if (!id) return E_INVALIDARG;
    *id = CLSID_D2D1GaussianBlur;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR n, UINT* index,
                                                    awge::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
    if (!n || !index || !mapping) return E_INVALIDARG;
    if (wcscmp(n, L"BlurAmount") != 0) return E_INVALIDARG;
    *index = D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
    *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override {
    if (!count) return E_INVALIDARG;
    *count = 3;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    try {
      wf::IInspectable v{nullptr};
      switch (index) {
        case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION: v = wf::PropertyValue::CreateSingle(amount); break;
        case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION:
          v = wf::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
          break;
        // HARD: clamp at the edges so the bar's border doesn't fade to transparent.
        case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE: v = wf::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD); break;
        default: return E_BOUNDS;
      }
      *value = v.as<ABI::Windows::Foundation::IPropertyValue>().detach();
      return S_OK;
    } catch (...) {
      return winrt::to_hresult();
    }
  }
  HRESULT STDMETHODCALLTYPE GetSource(UINT index, awge::IGraphicsEffectSource** out) noexcept override {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (index != 0) return E_BOUNDS;
    winrt::copy_to_abi(source, *reinterpret_cast<void**>(out));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override {
    if (!count) return E_INVALIDARG;
    *count = 1;
    return S_OK;
  }
};

struct BlurBrush : wuxm::XamlCompositionBrushBaseT<BlurBrush> {
  BlurBrush(wuc::Compositor compositor, float amount) : comp(std::move(compositor)), blur(amount) {}

  void OnConnected() {
    if (CompositionBrush()) return;
    try {
      auto fx = winrt::make_self<GaussianBlurEffect>();
      fx->source = wuc::CompositionEffectSourceParameter(L"backdrop");
      fx->amount = blur;
      auto brush = comp.CreateEffectFactory(*fx).CreateBrush();
      brush.SetSourceParameter(L"backdrop", comp.CreateBackdropBrush());
      CompositionBrush(brush);
    } catch (...) {
      Log(L"blur brush creation failed (%08X)", winrt::to_hresult().value);
    }
  }
  void OnDisconnected() {
    if (auto brush = CompositionBrush()) {
      try {
        brush.Close();
      } catch (...) {
      }
      CompositionBrush(nullptr);
    }
  }

  wuc::Compositor comp;
  float blur;
};

// Keys identify an appearance so an identical brush is not rebuilt.
uint64_t FillKey(const Appearance& a) {
  const uint32_t extra = a.kind == static_cast<uint32_t>(Kind::Blur) ? a.blur : 0;
  return (static_cast<uint64_t>(a.kind) << 32) | extra;
}
constexpr uint64_t kHiddenLineKey = 0xFFFF'0000'0000'0000ull;

void ApplyPartFill(Part& p, const Appearance* want, bool force) {
  if (!p.rect) return;
  if (!want || want->kind == static_cast<uint32_t>(Kind::Normal)) {
    SetPart(p, nullptr, 0);
    return;
  }
  const uint64_t key = FillKey(*want);
  if (!force && p.modified && p.oursKey == key) return;
  wuxm::Brush brush{nullptr};
  try {
    if (want->kind == static_cast<uint32_t>(Kind::Blur)) {
      auto compositor = wux::Hosting::ElementCompositionPreview::GetElementVisual(p.rect).Compositor();
      const uint32_t amount = want->blur < kMinBlur ? kDefaultBlur : (want->blur > kMaxBlur ? kMaxBlur : want->blur);
      brush = winrt::make<BlurBrush>(std::move(compositor), static_cast<float>(amount));
    } else {
      brush = wuxm::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0});  // Kind::Clear
    }
  } catch (...) {
    return;
  }
  SetPart(p, brush, key);
}

void ApplyPartLine(Part& p, const Appearance* want, bool force) {
  if (!p.rect) return;
  if (!want || want->showLine) {
    SetPart(p, nullptr, 0);
    return;
  }
  if (!force && p.modified && p.oursKey == kHiddenLineKey) return;
  try {
    SetPart(p, wuxm::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}), kHiddenLineKey);
  } catch (...) {
  }
}

void NotifyExeTaskbarFound() {
  static const UINT msg = RegisterWindowMessageW(kTaskbarFoundMsg);
  HWND w = FindWindowW(kWorkerClass, nullptr);
  if (w && msg) PostMessageW(w, msg, 0, 0);
}

// Must run on rec's UI thread.
void ApplyRec(Rec& rec, bool force) {
  ResolveHwnd(rec);
  Appearance want{};
  const bool have = LookupDesired(rec.hwnd, &want);
  if (force) {
    rec.fill.suspended = rec.stroke.suspended = false;
    rec.fill.burstCount = rec.stroke.burstCount = 0;
  }
  if (!have && rec.hwnd && !rec.notifiedExe) {
    bool alive;
    {
      SrwGuard g(&g_lock);
      alive = g_exeAlive;
    }
    if (alive) {
      rec.notifiedExe = true;
      NotifyExeTaskbarFound();
    }
  }
  if (have) rec.notifiedExe = false;
  ApplyPartFill(rec.fill, have ? &want : nullptr, force);
  ApplyPartLine(rec.stroke, have ? &want : nullptr, force);
}

void EnqueueApply(const std::shared_ptr<Rec>& rec, bool force,
                  ws::DispatcherQueuePriority prio = ws::DispatcherQueuePriority::Normal) {
  std::weak_ptr<Rec> weak = rec;
  try {
    rec->queue.TryEnqueue(prio, [weak, force]() {
      try {
        if (auto r = weak.lock()) ApplyRec(*r, force);
      } catch (...) {
      }
    });
  } catch (...) {
  }
}

void ApplyAll(bool force) {
  std::vector<std::shared_ptr<Rec>> recs;
  {
    SrwGuard g(&g_lock);
    recs = g_recs;
  }
  for (auto& r : recs) EnqueueApply(r, force);
}

// Explorer changed Fill under us (theme change, visual state, re-template).
void OnFillChanged(const std::weak_ptr<Rec>& weak, bool isFill) {
  auto rec = weak.lock();
  if (!rec) return;
  Part& p = isFill ? rec->fill : rec->stroke;
  if (p.applying || !p.modified || !p.rect || p.suspended) return;
  wf::IInspectable cur{nullptr};
  try {
    cur = p.rect.ReadLocalValue(wuxs::Shape::FillProperty());
  } catch (...) {
    return;
  }
  if (cur == p.ours) return;  // effective value changed but our local value stands
  const ULONGLONG now = GetTickCount64();
  if (now - p.burstStart > 3000) {
    p.burstStart = now;
    p.burstCount = 0;
  }
  if (++p.burstCount > 30) {
    p.suspended = true;  // stop fighting until ClearBar.exe sends a new state
    Log(L"frame %llx: explorer keeps resetting %s, backing off", rec->frameHandle, isFill ? L"fill" : L"line");
    return;
  }
  p.origLocal = cur;  // explorer's new value becomes the one to restore
  p.modified = false;
  EnqueueApply(rec, false);
}

// Stops tracking a rectangle. If our brush is still on it, put the original
// back first (deferred on its UI queue, Low priority so it runs before any
// re-registration's apply) - otherwise a rect that leaves and re-enters the
// tree would keep our brush and later "restore" to it.
void ResetPart(Part& p, const ws::DispatcherQueue& q) {
  if (p.rect && p.hasToken) {
    try {
      p.rect.UnregisterPropertyChangedCallback(wuxs::Shape::FillProperty(), p.token);
    } catch (...) {
    }
  }
  if (p.rect && p.modified && q) {
    auto rect = p.rect;
    auto orig = p.origLocal;
    auto ours = p.ours;
    try {
      q.TryEnqueue(ws::DispatcherQueuePriority::Low, [rect, orig, ours]() {
        try {
          const auto fp = wuxs::Shape::FillProperty();
          if (rect.ReadLocalValue(fp) != ours) return;  // explorer already replaced it
          if (!orig || orig == wux::DependencyProperty::UnsetValue()) rect.ClearValue(fp);
          else rect.SetValue(fp, orig);
        } catch (...) {
        }
      });
    } catch (...) {
    }
  }
  p = Part{};
}

// Called on the UI thread from OnVisualTreeChange.
void OnBackgroundRect(InstanceHandle handle, bool isFill) {
  const auto diag = Diag();
  if (!diag) return;
  wf::IInspectable insp;
  if (FAILED(diag->GetIInspectableFromHandle(handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(insp)))))
    return;
  auto rect = insp.try_as<wuxs::Rectangle>();
  if (!rect) return;

  wux::FrameworkElement frame{nullptr};
  for (auto p = wuxm::VisualTreeHelper::GetParent(rect); p; p = wuxm::VisualTreeHelper::GetParent(p)) {
    auto fe = p.try_as<wux::FrameworkElement>();
    if (!fe) continue;
    const auto cls = winrt::get_class_name(fe);
    if (cls == L"Taskbar.TaskbarBackground" && fe.Name() == L"HoverFlyoutBackgroundControl") return;
    if (cls == L"Taskbar.TaskbarFrame") {
      frame = fe;
      break;
    }
  }
  if (!frame) return;

  std::shared_ptr<Rec> rec;
  {
    SrwGuard g(&g_lock);
    for (auto& r : g_recs)
      if (r->frame == frame) rec = r;
  }
  if (!rec) {
    rec = std::make_shared<Rec>();
    rec->frame = frame;
    rec->queue = ws::DispatcherQueue::GetForCurrentThread();
    if (!rec->queue) return;
    diag->GetHandleFromIInspectable(reinterpret_cast<::IInspectable*>(winrt::get_abi(frame)), &rec->frameHandle);
    SrwGuard g(&g_lock);
    g_recs.push_back(rec);
  }
  Part& part = isFill ? rec->fill : rec->stroke;
  if (part.rect == rect) return;
  ResetPart(part, rec->queue);
  part.rect = rect;
  part.handle = handle;
  std::weak_ptr<Rec> weak = rec;
  try {
    part.token = rect.RegisterPropertyChangedCallback(
        wuxs::Shape::FillProperty(), [weak, isFill](wux::DependencyObject const&, wux::DependencyProperty const&) {
          try {
            OnFillChanged(weak, isFill);
          } catch (...) {
          }
        });
    part.hasToken = true;
  } catch (...) {
  }
  Log(L"frame %llx: %s rect %llx registered", rec->frameHandle, isFill ? L"BackgroundFill" : L"BackgroundStroke",
      handle);
  // Low priority: setting Fill before TaskbarBackground's template is applied
  // can crash or be overwritten.
  EnqueueApply(rec, true, ws::DispatcherQueuePriority::Low);
}

// Called on the removed element's UI thread, which is also the thread that
// owns any Rec referencing it, so XAML calls below are on the right thread.
void OnRemoved(InstanceHandle handle) {
  std::shared_ptr<Rec> gone;
  std::vector<std::shared_ptr<Rec>> recs;
  {
    SrwGuard g(&g_lock);
    for (auto it = g_sources.begin(); it != g_sources.end(); ++it)
      if (*it == handle) {
        g_sources.erase(it);
        break;
      }
    for (auto it = g_recs.begin(); it != g_recs.end(); ++it)
      if ((*it)->frameHandle == handle) {
        gone = *it;
        g_recs.erase(it);
        break;
      }
    if (!gone) recs = g_recs;
  }
  if (gone) {  // frame going away (monitor removed / explorer rebuilding)
    ResetPart(gone->fill, gone->queue);
    ResetPart(gone->stroke, gone->queue);
    Log(L"frame %llx removed", handle);
    return;
  }
  for (auto& r : recs) {
    if (r->fill.handle == handle) ResetPart(r->fill, r->queue);
    if (r->stroke.handle == handle) ResetPart(r->stroke, r->queue);
  }
}

// ------------------------------------------------------ XAML diagnostics
struct BstrGuard {
  BSTR b;
  ~BstrGuard() { if (b) SysFreeString(b); }
};

struct Watcher : winrt::implements<Watcher, IVisualTreeServiceCallback2, winrt::non_agile> {
  HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element,
                                               VisualMutationType mutation) noexcept override {
    BstrGuard f{element.SrcInfo.FileName}, h{element.SrcInfo.Hash}, n{element.Name}, t{element.Type};
    try {
      const wchar_t* type = element.Type ? element.Type : L"";
      const wchar_t* name = element.Name ? element.Name : L"";
      if (mutation == Add) {
        if (g_flags.load(std::memory_order_relaxed) & kFlagTraceTree)
          Log(L"+ %llx parent %llx %s #%s", element.Handle, relation.Parent, type, name);
        if (wcscmp(type, L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource") == 0) {
          SrwGuard g(&g_lock);
          g_sources.push_back(element.Handle);
        } else if (wcscmp(type, L"Windows.UI.Xaml.Shapes.Rectangle") == 0) {
          if (wcscmp(name, L"BackgroundFill") == 0) OnBackgroundRect(element.Handle, true);
          else if (wcscmp(name, L"BackgroundStroke") == 0) OnBackgroundRect(element.Handle, false);
        }
      } else if (mutation == Remove) {
        OnRemoved(element.Handle);
      }
    } catch (...) {
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override {
    return S_OK;
  }
};

winrt::com_ptr<Watcher>& g_watcher = *new winrt::com_ptr<Watcher>();

DWORD WINAPI AdviseThread(LPVOID) {
  HRESULT hr = E_FAIL;
  winrt::com_ptr<IVisualTreeService3> service;
  winrt::com_ptr<Watcher> watcher;
  {
    SrwGuard g(&g_lock);
    watcher = g_watcher;
    if (g_diag) service = g_diag.try_as<IVisualTreeService3>();
  }
  if (service && watcher) {
    t_fakeDiagDisabled = true;
    hr = service->AdviseVisualTreeChange(watcher.get());
    t_fakeDiagDisabled = false;
  }
  Log(L"AdviseVisualTreeChange -> %08X", hr);
  if (SUCCEEDED(hr)) {
    if (g_readyEvent) SetEvent(g_readyEvent);
  } else {
    // Undo any partial registration, then allow a later install attempt to
    // create a fresh site. g_diag/g_watcher are kept (a still-queued callback
    // may read them); the next SetSite replaces them under g_lock.
    if (service && watcher) {
      try {
        service->UnadviseVisualTreeChange(watcher.get());
      } catch (...) {
      }
    }
    g_siteSet.store(false);
  }
  return 0;
}

struct TapSite : winrt::implements<TapSite, IObjectWithSite> {
  winrt::com_ptr<IUnknown> site;
  HRESULT STDMETHODCALLTYPE SetSite(IUnknown* s) noexcept override {
    if (!s) {
      site = nullptr;
      return S_OK;
    }
    if (g_siteSet.exchange(true)) return S_OK;  // one watcher per process
    try {
      site.copy_from(s);
      auto diag = site.as<IXamlDiagnostics>();
      auto watcher = winrt::make_self<Watcher>();
      {
        SrwGuard g(&g_lock);
        g_diag = diag;
        g_watcher = watcher;
      }
      // Advising from this thread can hang in RunOnUIThread; use a new one.
      HANDLE t = CreateThread(nullptr, 0, AdviseThread, nullptr, 0, nullptr);
      if (t) CloseHandle(t);
      return S_OK;
    } catch (...) {
      return winrt::to_hresult();
    }
  }
  HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** out) noexcept override {
    if (!out) return E_POINTER;
    *out = nullptr;
    return site ? site->QueryInterface(riid, out) : E_FAIL;
  }
};

struct Factory : winrt::implements<Factory, IClassFactory> {
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** out) noexcept override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    try {
      return winrt::make<TapSite>().as<IUnknown>()->QueryInterface(riid, out);
    } catch (...) {
      return winrt::to_hresult();
    }
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

// ------------------------------------------------- state from ClearBar.exe
DWORD WINAPI StateThread(LPVOID) {
  HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, kChangedEventName);
  if (!changed) return 1;
  HANDLE map = nullptr;
  const Shared* view = nullptr;
  HANDLE exeProc = nullptr;
  DWORD exePid = 0;
  uint32_t lastGen = 0;
  bool first = true;
  for (;;) {
    DWORD r = WAIT_OBJECT_0;
    if (!first) {
      HANDLE hs[2] = {changed, exeProc};
      r = WaitForMultipleObjects(exeProc ? 2 : 1, hs, FALSE, INFINITE);
    }
    first = false;
    if (r == WAIT_OBJECT_0) {
      if (!view) {
        map = OpenFileMappingW(FILE_MAP_READ, FALSE, kMapName);
        if (map) view = static_cast<const Shared*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(Shared)));
        if (!view && map) {
          CloseHandle(map);
          map = nullptr;
        }
      }
      Shared snap;
      if (!view || !ReadSnapshot(view, &snap)) continue;
      if (!Validate(snap)) {
        static bool warned = false;
        if (!warned && snap.magic == kMagic && snap.version != kProtocolVersion) {
          warned = true;
          g_flags.store(kFlagDebugLog, std::memory_order_relaxed);  // make this one line visible
          Log(L"ClearBar.exe speaks protocol v%u, this TAP v%u: restart explorer", snap.version, kProtocolVersion);
          g_flags.store(0, std::memory_order_relaxed);
        }
        continue;
      }
      if (snap.exePid != exePid || !exeProc) {
        if (exeProc) CloseHandle(exeProc);
        exePid = snap.exePid;
        exeProc = OpenProcess(SYNCHRONIZE, FALSE, exePid);
      }
      g_flags.store(snap.flags, std::memory_order_relaxed);
      const bool force = snap.generation != lastGen;
      lastGen = snap.generation;
      {
        SrwGuard g(&g_lock);
        g_snap = snap;
        g_haveSnap = true;
        g_exeAlive = exeProc != nullptr;
      }
      ApplyAll(force);
    } else if (r == WAIT_OBJECT_0 + 1) {
      Log(L"ClearBar.exe (pid %lu) exited; restoring taskbars", exePid);
      CloseHandle(exeProc);
      exeProc = nullptr;
      exePid = 0;
      {
        SrwGuard g(&g_lock);
        g_exeAlive = false;
      }
      ApplyAll(true);
    } else {
      Sleep(1000);
    }
  }
}

std::atomic<bool> g_installing{false};
std::atomic<ULONGLONG> g_lastInstallStart{0};

bool TaskbarXamlReady() {
  HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (!tray) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(tray, &pid);
  return pid == GetCurrentProcessId() &&
         FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) != nullptr;
}

DWORD WINAPI InstallThread(LPVOID) {
  struct Done {
    ~Done() { g_installing.store(false); }
  } done;
  Sleep(50);  // let the first state read land before the tree replay

  HMODULE wux = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!wux) {
    Log(L"Windows.UI.Xaml.dll not loadable");
    return 1;
  }
  InstallRegistryShim(wux);
  using Ixde = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
  auto ixde = reinterpret_cast<Ixde>(GetProcAddress(wux, "InitializeXamlDiagnosticsEx"));
  if (!ixde) {
    Log(L"InitializeXamlDiagnosticsEx missing");
    return 1;
  }
  wchar_t self[MAX_PATH * 2];
  const DWORD len = GetModuleFileNameW(g_module, self, _countof(self));
  if (!len || len >= _countof(self)) return 1;

  // 1) Wait until the taskbar's XAML island exists (explorer starting up).
  //    Before that there is no XAML core to connect to.
  for (int i = 0; i < 360 && !TaskbarXamlReady(); ++i) Sleep(250);  // <= 90 s
  if (!TaskbarXamlReady()) {
    Log(L"taskbar XAML island never appeared; giving up");
    return 1;
  }

  // 2) Every XAML core (one per XAML UI thread) registers the port
  //    "VisualDiagConnection<N>", N = 1,2,3... in creation order
  //    (DXamlCore::RegisterVisualDiagnosticsPort). ERROR_NOT_FOUND only means
  //    "no live core with that number", so scan upward from 1 every round and
  //    connect to the first live one. NOT_FOUND calls don't consume IXDE's
  //    once-per-thread initialisation, so one fresh thread per round suffices.
  struct Attempt {
    Ixde fn;
    const wchar_t* dll;
    wchar_t endpoint[64];
    HRESULT hr;
  } a{ixde, self, {}, E_FAIL};
  for (int round = 0; round < 30 && !g_siteSet.load(); ++round) {
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
      auto* at = static_cast<Attempt*>(p);
      for (int n = 1; n <= 4096; ++n) {
        swprintf_s(at->endpoint, L"VisualDiagConnection%d", n);
        at->hr = at->fn(at->endpoint, GetCurrentProcessId(), nullptr, at->dll, CLSID_ClearBarTap, nullptr);
        if (at->hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
      }
      return 0;
    }, &a, 0, nullptr);
    if (!t) break;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    if (SUCCEEDED(a.hr)) {
      Log(L"InitializeXamlDiagnosticsEx(%s) ok", a.endpoint);
      return 0;
    }
    Log(L"InitializeXamlDiagnosticsEx round %d: %s -> %08X", round, a.endpoint, a.hr);
    Sleep(1000);
  }
  Log(L"giving up on XAML diagnostics");
  return 1;
}

bool IsHostExplorer() {
  wchar_t win[MAX_PATH], exe[MAX_PATH * 2];
  const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
  const DWORD m = GetModuleFileNameW(nullptr, exe, _countof(exe));
  if (!n || n >= MAX_PATH || !m || m >= _countof(exe)) return false;
  wcscat_s(win, L"\\explorer.exe");
  return _wcsicmp(win, exe) == 0;
}

void StartInExplorer() {
  const DWORD pid = GetCurrentProcessId();
  wchar_t name[128];
  swprintf_s(name, L"%s%lu", kInstalledMutexPrefix, pid);
  HANDLE installed = CreateMutexW(nullptr, FALSE, name);  // intentionally never closed
  if (!installed) return;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(installed);  // another copy already runs in this explorer
    return;
  }
  swprintf_s(name, L"%s%lu", kReadyEventPrefix, pid);
  g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, name);  // kept for explorer's lifetime

  // Debug log goes next to the DLL (only written when enabled in config).
  const DWORD len = GetModuleFileNameW(g_module, g_logPath, _countof(g_logPath));
  if (len && len < _countof(g_logPath)) {
    wchar_t* slash = wcsrchr(g_logPath, L'\\');
    if (slash) wcscpy_s(slash + 1, _countof(g_logPath) - (slash + 1 - g_logPath), L"tap.log");
    else g_logPath[0] = 0;
  } else {
    g_logPath[0] = 0;
  }

  // Stay loaded after the installer hook is removed.
  HMODULE pinned;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(&StartInExplorer), &pinned);
  HANDLE st = CreateThread(nullptr, 0, StateThread, nullptr, 0, nullptr);  // once per explorer
  if (st) CloseHandle(st);
  g_installing.store(true);
  g_lastInstallStart.store(GetTickCount64());
  HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  else g_installing.store(false);
}

}  // namespace

// --------------------------------------------------------------- exports
// Re-runs a failed install. Returns FALSE if this module is not the copy that
// owns the install in this explorer (a ClearBarTap.dll from another folder).
extern "C" BOOL WINAPI ClearBarTryRetry() {
  if (!g_readyEvent) return FALSE;
  if (!g_siteSet.load() && !g_installing.load() &&
      GetTickCount64() - g_lastInstallStart.load() > 5000 &&  // never hammer explorer
      WaitForSingleObject(g_readyEvent, 0) != WAIT_OBJECT_0 && !g_installing.exchange(true)) {
    g_lastInstallStart.store(GetTickCount64());
    HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else g_installing.store(false);
  }
  return TRUE;
}

namespace {
using TryRetryFn = BOOL(WINAPI*)();
// For a non-owning copy: find the owning ClearBarTap.dll loaded in explorer.
TryRetryFn OwnerRetry() {
  static TryRetryFn owner = nullptr;
  static bool searched = false;  // hook proc runs on one thread only
  if (searched) return owner;
  searched = true;
  HMODULE mods[1024];
  DWORD needed = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
  const DWORD count = (needed < sizeof(mods) ? needed : sizeof(mods)) / sizeof(HMODULE);
  for (DWORD i = 0; i < count; ++i) {
    if (mods[i] == g_module) continue;
    wchar_t name[MAX_PATH];
    if (!K32GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
    if (_wcsicmp(name, L"ClearBarTap.dll") != 0) continue;
    auto fn = reinterpret_cast<TryRetryFn>(GetProcAddress(mods[i], "ClearBarTryRetry"));
    if (fn && fn()) {  // TRUE only from the owner
      owner = fn;
      break;
    }
  }
  return owner;
}
}  // namespace

// Runs only on explorer's taskbar thread while ClearBar.exe holds the hook.
// The first load installs from DllMain; if that install failed, a later hook
// (ClearBar.exe retrying) re-runs it - forwarded to the owning copy if this
// is a second copy of the DLL from a different folder.
extern "C" LRESULT CALLBACK ClearBarHookProc(int code, WPARAM wp, LPARAM lp) {
  if (code >= 0) {
    if (g_readyEvent) ClearBarTryRetry();
    else if (TryRetryFn owner = OwnerRetry()) owner();
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID clsid, _In_ REFIID riid, _Outptr_ LPVOID* out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (clsid != CLSID_ClearBarTap) return CLASS_E_CLASSNOTAVAILABLE;
  try {
    return winrt::make<Factory>().as<IUnknown>()->QueryInterface(riid, out);
  } catch (...) {
    return winrt::to_hresult();
  }
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow() { return S_FALSE; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
    if (IsHostExplorer()) StartInExplorer();
  }
  return TRUE;
}
