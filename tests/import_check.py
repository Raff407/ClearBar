"""Fail if a binary imports an API not known to exist on Windows 10 2004 (19041).

Usage: python -I tests/import_check.py out/ClearBar.exe out/ClearBarTap.dll

ClearBar is meant to run on Windows 10 19041+ as well as Windows 11. A new API
that only exists on a later build would make it fail to load on 19041 with no
warning, so the import tables are pinned to an allowlist here. The allowlist is
every DLL!function the binaries imported when it was written; all of these ship
in Windows 10 2004. Adding an import (for the Windows 10 backend or anything
else) is a deliberate step: confirm the API exists on 19041, then add it below.

Parses the PE import and delay-import directories directly (no dumpbin), so it
runs anywhere Python does.
"""
import struct
import sys

# DLL (lowercased) -> set of allowed imports. Ordinal imports are "#<n>".
ALLOW = {
    "kernel32.dll": {
        "AcquireSRWLockExclusive", "CloseHandle", "CreateEventW", "CreateFileMappingW",
        "CreateFileW", "CreateMutexW", "CreateThread", "DeleteCriticalSection",
        "DisableThreadLibraryCalls", "EncodePointer", "EnterCriticalSection", "ExitProcess",
        "FindClose", "FindFirstChangeNotificationW", "FindFirstFileExW",
        "FindNextChangeNotification", "FindNextFileW", "FlsAlloc", "FlsFree", "FlsGetValue",
        "FlsSetValue", "FlushFileBuffers", "FreeEnvironmentStringsW", "FreeLibrary", "GetACP",
        "GetCPInfo", "GetCommandLineA", "GetCommandLineW", "GetConsoleMode",
        "GetConsoleOutputCP", "GetCurrentProcess", "GetCurrentProcessId", "GetCurrentThreadId",
        "GetEnvironmentStringsW", "GetFileAttributesW", "GetFileSizeEx", "GetFileType",
        "GetLastError", "GetLocalTime", "GetModuleFileNameW", "GetModuleHandleExW",
        "GetModuleHandleW", "GetOEMCP", "GetProcAddress", "GetProcessHeap", "GetProcessTimes",
        "GetStartupInfoW", "GetStdHandle", "GetStringTypeW", "GetSystemTimeAsFileTime",
        "GetTickCount64", "GetWindowsDirectoryW", "HeapAlloc", "HeapFree", "HeapReAlloc",
        "HeapSize", "InitializeCriticalSectionEx", "InitializeSListHead",
        "InterlockedFlushSList", "InterlockedPushEntrySList", "IsDebuggerPresent",
        "IsProcessorFeaturePresent", "IsValidCodePage", "K32EnumProcessModules",
        "K32GetModuleBaseNameW", "LCMapStringW", "LeaveCriticalSection", "LoadLibraryExW",
        "MapViewOfFile", "MultiByteToWideChar", "OpenEventW", "OpenFileMappingW", "OpenProcess",
        "QueryFullProcessImageNameW", "QueryPerformanceCounter", "RaiseException", "ReadFile",
        "ReleaseSRWLockExclusive", "RtlCaptureContext", "RtlLookupFunctionEntry",
        "RtlPcToFileHeader", "RtlUnwindEx", "RtlVirtualUnwind", "SetEvent", "SetFilePointerEx",
        "SetLastError", "SetProcessInformation", "SetStdHandle", "SetUnhandledExceptionFilter",
        "Sleep", "SleepConditionVariableSRW", "TerminateProcess", "UnhandledExceptionFilter",
        "VirtualProtect", "WaitForMultipleObjects", "WaitForSingleObject",
        "WakeAllConditionVariable", "WideCharToMultiByte", "WriteConsoleW", "WriteFile",
    },
    "user32.dll": {
        "CallNextHookEx", "ChangeWindowMessageFilterEx", "CreateWindowExW", "DefWindowProcW",
        "DestroyWindow", "DispatchMessageW", "EnumWindows", "EqualRect", "FindWindowExW",
        "FindWindowW", "GetAncestor", "GetClassNameW", "GetForegroundWindow", "GetMonitorInfoW",
        "GetWindowLongPtrW", "GetWindowRect", "GetWindowTextW", "GetWindowThreadProcessId",
        "IsIconic", "IsRectEmpty", "IsWindow", "IsWindowVisible", "IsZoomed", "KillTimer",
        "MonitorFromPoint", "MonitorFromWindow", "MsgWaitForMultipleObjectsEx", "PeekMessageW",
        "PostMessageW", "PostQuitMessage", "RegisterClassExW", "RegisterWindowMessageW",
        "SendMessageTimeoutW",
        "SetProcessDpiAwarenessContext", "SetTimer", "SetWinEventHook", "SetWindowsHookExW",
        "UnhookWinEvent", "UnhookWindowsHookEx",
    },
    "dwmapi.dll": {"DwmGetWindowAttribute"},
    "wtsapi32.dll": {"WTSRegisterSessionNotification"},
    "oleaut32.dll": {"#6", "#7", "#200", "#201"},
}
# API sets imported today (exact contract versions). Each of these versions ships
# in Windows 10 2004. A newer contract version (e.g. -l1-2-2) is a new name and
# fails the check until someone confirms it exists on 19041 and adds it here.
ALLOW_APISETS = {
    "api-ms-win-core-com-l1-1-0.dll", "api-ms-win-core-console-l1-1-0.dll",
    "api-ms-win-core-debug-l1-1-0.dll", "api-ms-win-core-errorhandling-l1-1-0.dll",
    "api-ms-win-core-fibers-l1-1-0.dll", "api-ms-win-core-file-l1-1-0.dll",
    "api-ms-win-core-handle-l1-1-0.dll", "api-ms-win-core-heap-l1-1-0.dll",
    "api-ms-win-core-interlocked-l1-1-0.dll", "api-ms-win-core-libraryloader-l1-2-0.dll",
    "api-ms-win-core-localization-l1-2-0.dll", "api-ms-win-core-memory-l1-1-0.dll",
    "api-ms-win-core-processenvironment-l1-1-0.dll", "api-ms-win-core-processthreads-l1-1-0.dll",
    "api-ms-win-core-processthreads-l1-1-1.dll", "api-ms-win-core-psapi-l1-1-0.dll",
    "api-ms-win-core-rtlsupport-l1-1-0.dll", "api-ms-win-core-string-l1-1-0.dll",
    "api-ms-win-core-synch-l1-1-0.dll", "api-ms-win-core-synch-l1-2-0.dll",
    "api-ms-win-core-synch-l1-2-1.dll", "api-ms-win-core-sysinfo-l1-1-0.dll",
    "api-ms-win-core-util-l1-1-0.dll", "api-ms-win-core-winrt-error-l1-1-1.dll",
    "api-ms-win-core-winrt-l1-1-0.dll",
}


def _imports(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e:e + 4] != b"PE\0\0":
        raise SystemExit(path + ": not a PE file")
    opt = e + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    if magic != 0x20B:
        raise SystemExit(path + ": not a 64-bit PE")
    dd = opt + 112
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    secoff = opt + struct.unpack_from("<H", d, e + 20)[0]
    secs = []
    for i in range(nsec):
        o = secoff + 40 * i
        va = struct.unpack_from("<I", d, o + 12)[0]
        vs = struct.unpack_from("<I", d, o + 8)[0]
        raw = struct.unpack_from("<I", d, o + 20)[0]
        rs = struct.unpack_from("<I", d, o + 16)[0]
        secs.append((va, max(vs, rs), raw))

    def r2o(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return raw + (rva - va)
        return None

    def cstr(o):
        return d[o:d.index(b"\0", o)].decode("ascii", "replace")

    out = {}

    def thunks(int_rva, bucket):
        o = r2o(int_rva)
        j = 0
        while True:
            v = struct.unpack_from("<Q", d, o + 8 * j)[0]
            j += 1
            if not v:
                break
            if v & (1 << 63):
                bucket.add("#%d" % (v & 0xFFFF))
            else:
                bucket.add(cstr(r2o((v & 0xFFFFFFFF) + 2)))

    # Import directory (index 1): 20-byte descriptors, OriginalFirstThunk at +0.
    rva = struct.unpack_from("<I", d, dd + 8)[0]
    k = 0
    while rva:
        o = r2o(rva + 20 * k)
        k += 1
        if o is None:
            break
        name_rva = struct.unpack_from("<I", d, o + 12)[0]
        if not name_rva:
            break
        int_rva = struct.unpack_from("<I", d, o + 0)[0] or struct.unpack_from("<I", d, o + 16)[0]
        thunks(int_rva, out.setdefault(cstr(r2o(name_rva)).lower(), set()))

    # Delay-import directory (index 13): 32-byte descriptors, DllNameRVA at +4.
    drva = struct.unpack_from("<I", d, dd + 13 * 8)[0]
    k = 0
    while drva:
        o = r2o(drva + 32 * k)
        k += 1
        if o is None:
            break
        name_rva = struct.unpack_from("<I", d, o + 4)[0]
        if not name_rva:
            break
        int_rva = struct.unpack_from("<I", d, o + 16)[0]
        if int_rva:
            thunks(int_rva, out.setdefault(cstr(r2o(name_rva)).lower(), set()))
    return out


def check(path):
    bad = []
    for dll, fns in _imports(path).items():
        if dll in ALLOW_APISETS:
            continue
        allowed = ALLOW.get(dll)
        if allowed is None:
            bad += ["%s!%s  (DLL not on the Windows 10 allowlist)" % (dll, f) for f in sorted(fns)]
            continue
        bad += ["%s!%s" % (dll, f) for f in sorted(fns - allowed)]
    return bad


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    fails = 0
    for path in argv:
        bad = check(path)
        if bad:
            fails += 1
            print("FAIL %s imports not on the Windows 10 19041 allowlist:" % path)
            for b in bad:
                print("  " + b)
        else:
            print("OK   %s" % path)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
