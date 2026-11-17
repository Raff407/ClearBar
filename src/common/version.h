#pragma once
#define CLEARBAR_VERSION_MAJOR 1
#define CLEARBAR_VERSION_MINOR 1
#define CLEARBAR_VERSION_PATCH 0
#define CLEARBAR_VERSION_STR "1.1.0"
// ClearBarTap.dll has its own version: it only changes when tap.cpp or the
// shared-memory protocol changes. An unchanged DLL stays byte-identical, so
// upgrading does not need an Explorer restart (install.ps1 compares hashes).
#define CLEARBAR_TAP_VERSION_MAJOR 1
#define CLEARBAR_TAP_VERSION_MINOR 0
#define CLEARBAR_TAP_VERSION_PATCH 0
#define CLEARBAR_TAP_VERSION_STR "1.0.0"
