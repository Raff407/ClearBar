// Windows 10 taskbar looks (pure, no side effects; used by main.cpp and tests).
//
// Windows 10 draws its taskbar with DWM, not XAML. Its look is the window's
// "accent policy", set with user32's SetWindowCompositionAttribute (exported by
// name, undocumented, the API Windows 10 apps and TranslucentTB use). ClearBar.exe
// sets it on Explorer's taskbar windows from its own process: nothing is loaded
// into Explorer on Windows 10.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../common/protocol.h"

namespace cb::accent {

enum AccentState : int32_t {
  ACCENT_DISABLED = 0,
  ACCENT_ENABLE_GRADIENT = 1,
  ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,  // GradientColor 0x00000000 = fully clear
  ACCENT_ENABLE_BLURBEHIND = 3,           // DWM blur, fixed radius
  ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};

struct AccentPolicy {
  int32_t AccentState;
  uint32_t AccentFlags;
  uint32_t GradientColor;  // 0xAABBGGRR
  int32_t AnimationId;
};

struct WindowCompositionAttribData {
  int32_t Attrib;
  void* pvData;
  size_t cbData;  // SIZE_T
};

static_assert(sizeof(AccentPolicy) == 16, "ACCENT_POLICY is 16 bytes");
static_assert(sizeof(void*) != 8 || sizeof(WindowCompositionAttribData) == 24, "x64 layout");
static_assert(sizeof(void*) != 8 || offsetof(WindowCompositionAttribData, pvData) == 8, "x64 layout");
static_assert(sizeof(void*) != 8 || offsetof(WindowCompositionAttribData, cbData) == 16, "x64 layout");

inline constexpr int32_t kWcaAccentPolicy = 19;          // WCA_ACCENT_POLICY
inline constexpr uint32_t kAccentFlagGradientColor = 2;  // GradientColor is used
// Sent to a taskbar to make Explorer re-apply its own accent (Windows' normal look).
inline constexpr unsigned kMsgDwmCompositionChanged = 0x031E;  // WM_DWMCOMPOSITIONCHANGED

inline bool operator==(const AccentPolicy& a, const AccentPolicy& b) {
  return a.AccentState == b.AccentState && a.AccentFlags == b.AccentFlags &&
         a.GradientColor == b.GradientColor && a.AnimationId == b.AnimationId;
}

// True when ClearBar draws this look itself (clear or blur); false = Windows' own look.
inline bool IsCustom(const Appearance& a) {
  return a.kind == static_cast<uint32_t>(Kind::Clear) || a.kind == static_cast<uint32_t>(Kind::Blur);
}

// The accent for a custom look. Windows 10's blur radius is fixed by DWM, so
// a.blur is ignored; Windows 10 has no top line, so a.showLine is ignored.
inline AccentPolicy Win10Accent(const Appearance& a) {
  if (a.kind == static_cast<uint32_t>(Kind::Blur))
    return AccentPolicy{ACCENT_ENABLE_BLURBEHIND, kAccentFlagGradientColor, 0x00000000u, 0};
  return AccentPolicy{ACCENT_ENABLE_TRANSPARENTGRADIENT, kAccentFlagGradientColor, 0x00000000u, 0};
}

}  // namespace cb::accent
