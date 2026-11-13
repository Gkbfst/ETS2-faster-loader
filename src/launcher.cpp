// ets2fast.exe / inject.exe
//   ets2fast.exe --launch <game exe> [game args...]   used from Steam launch options
//   ets2fast.exe                                      wait for the game, then inject
//
// Both modes load ets2fast.dll into eurotrucks2.exe. The DLL finds the workshop
// folder by itself, so nothing here is machine specific.

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdarg>

static std::wstring g_dir;      // folder holding this exe
static std::wstring g_logPath;
static bool g_console = false;

// ---------------- logging ----------------
static void say(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;

    if (g_console) {
        wprintf(L"%ls\n", buf);
        fflush(stdout);
    }

    FILE* f = _wfopen(g_logPath.c_str(), L"a, ccs=UTF-8");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"%02d:%02d:%02d  %ls\n", st.wHour, st.wMinute, st.wSecond, buf);
        fclose(f);
    }
}

// ---------------- command line helpers ----------------
static const wchar_t* skip_ws(const wchar_t* p)
{
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

static const wchar_t* skip_token(const wchar_t* p)
{
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') ++p;
        if (*p == L'"') ++p;
        return p;
    }
    while (*p && *p != L' ' && *p != L'\t') ++p;
    return p;
}

static std::wstring token_str(const wchar_t* b, const wchar_t* e)
{
    if (b < e && *b == L'"') {
        ++b;
        if (e > b && e[-1] == L'"') --e;
    }
    return std::wstring(b, (size_t)(e - b));
}

// ---------------- process / injection ----------------
static DWORD find_proc(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool inject_once(DWORD pid, const std::wstring& dll)
{
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) {
        say(L"  OpenProcess failed: %lu%s", GetLastError(),
            GetLastError() == 5 ? L" (try run as administrator)" : L"");
        return false;
    }

    SIZE_T sz = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(h, nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        say(L"  VirtualAllocEx failed: %lu", GetLastError());
        CloseHandle(h);
        return false;
    }

    if (!WriteProcessMemory(h, mem, dll.c_str(), sz, nullptr)) {
        say(L"  WriteProcessMemory failed: %lu", GetLastError());
        VirtualFreeEx(h, mem, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    FARPROC load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(h, nullptr, 0, (LPTHREAD_START_ROUTINE)load, mem, 0, nullptr);

    bool ok = false;
    if (th) {
        DWORD w = WaitForSingleObject(th, 15000);
        DWORD code = 0;
        GetExitCodeThread(th, &code);
        ok = (w == WAIT_OBJECT_0 && code != 0 && code != STILL_ACTIVE);
        if (!ok) say(L"  remote LoadLibrary returned 0x%lX", code);
        CloseHandle(th);
    } else {
        say(L"  CreateRemoteThread failed: %lu", GetLastError());
    }

    VirtualFreeEx(h, mem, 0, MEM_RELEASE);
    CloseHandle(h);
    return ok;
}

// try until the deadline; the target may still be starting up
static bool inject_retry(DWORD pid, const std::wstring& dll, int seconds)
{
    for (int i = 0; i < seconds * 8; ++i) {
        if (inject_once(pid, dll)) return true;
        Sleep(125);
    }
    return false;
}

static long long file_size(const std::wstring& p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((long long)d.nFileSizeHigh << 32) | (long long)d.nFileSizeLow;
}

// read the log the DLL writes, to confirm the hook really is live.
// only the part appended after 'fromOffset' is inspected, so that a stale
// "hook installed" line from a previous session is not mistaken for this one.
static bool hook_is_live(const std::wstring& dllLog, long long fromOffset, int waitSeconds)
{
    for (int i = 0; i < waitSeconds * 4; ++i) {
        FILE* f = _wfopen(dllLog.c_str(), L"rb");
        if (f) {
            if (fromOffset > 0) _fseeki64(f, fromOffset, SEEK_SET);
            char buf[16384] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;

            // order matters: "root: NOT FOUND - no hook installed" also
            // contains the substring "hook installed"
            if (strstr(buf, "no hook installed") || strstr(buf, "root: NOT FOUND")) return false;
            if (strstr(buf, "hook installed")) return true;
        }
        Sleep(250);
    }
    return false;
}

// ---------------- modes ----------------
static int launch_mode(const std::wstring& gameExe, const std::wstring& gameArgs)
{
    if (GetFileAttributesW(gameExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        say(L"game exe not found: %ls", gameExe.c_str());
        return 2;
    }

    std::wstring cmd = L"\"" + gameExe + L"\"";
    if (!gameArgs.empty()) cmd += L" " + gameArgs;

    // working directory = folder of the game exe; tolerate both separators
    std::wstring cwd;
    size_t sep = gameExe.find_last_of(L"\\/");
    if (sep != std::wstring::npos) cwd = gameExe.substr(0, sep);
    if (!cwd.empty() && GetFileAttributesW(cwd.c_str()) == INVALID_FILE_ATTRIBUTES) cwd.clear();

    say(L"launching: %ls", cmd.c_str());

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    if (!CreateProcessW(gameExe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                        0, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        say(L"CreateProcess failed: %lu", GetLastError());
        return 3;
    }

    // tie the game to this process: if the launcher is killed (Steam "Stop"),
    // the game goes with it instead of lingering as an orphan
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;
        ZeroMemory(&jl, sizeof(jl));
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
        if (!AssignProcessToJobObject(job, pi.hProcess))
            say(L"note: could not join the job object (%lu), continuing anyway", GetLastError());
    }

    std::wstring dll = g_dir + L"ets2fast.dll";
    std::wstring dllLog = g_dir + L"ets2fast.log";
    long long mark = file_size(dllLog);
    bool ok = inject_retry(pi.dwProcessId, dll, 10);

    if (ok) {
        bool live = hook_is_live(dllLog, mark, 12);
        say(live ? L"hook is live - lookups served from memory"
                 : L"injected, but the hook did not confirm - see ets2fast.log");
    } else {
        say(L"injection failed - the game runs normally, just without the speedup");
    }

    // Steam waits for this process, so stay alive until the game really exits
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    say(L"game exited with code %lu", code);
    return (int)code;
}

static int watch_mode()
{
    say(L"DLL    : %ls", (g_dir + L"ets2fast.dll").c_str());
    say(L"waiting for eurotrucks2.exe (up to 180 s)...");
    if (g_console) wprintf(L"Start Euro Truck Simulator 2 from Steam now.\n\n");

    DWORD pid = 0;
    for (int i = 0; i < 1800; ++i) {
        pid = find_proc(L"eurotrucks2.exe");
        if (pid) break;
        Sleep(100);
    }
    if (!pid) {
        say(L"timeout - game process never appeared");
        return 2;
    }

    say(L"found eurotrucks2.exe (pid %lu), injecting...", pid);
    std::wstring dllLog = g_dir + L"ets2fast.log";
    long long mark = file_size(dllLog);
    if (!inject_retry(pid, g_dir + L"ets2fast.dll", 10)) {
        say(L"injection failed");
        return 3;
    }
    bool live = hook_is_live(dllLog, mark, 12);
    say(live ? L"OK - hook is active" : L"injected, hook not confirmed - see ets2fast.log");

    if (g_console) {
        wprintf(L"\n%ls\nPress Enter to close...\n", live ? L"OK - hook is active" : L"see ets2fast.log");
        getchar();
    }
    return live ? 0 : 4;
}

// ---------------- daemon ----------------
// Stays in the background and injects into eurotrucks2.exe every time it starts.
// This is what "auto inject" uses: it never touches Steam's own configuration,
// works no matter how the game is launched, and keeps the Steam overlay intact
// (which a wrapper through %command% would break).
#define ETS2FAST_MUTEX L"Local\\ets2fast_daemon_mutex"
#define ETS2FAST_STOP  L"Local\\ets2fast_daemon_stop"

static int daemon_mode()
{
    // one daemon per session; a second copy just leaves
    HANDLE single = CreateMutexW(nullptr, FALSE, ETS2FAST_MUTEX);
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        say(L"daemon: another instance is already running");
        CloseHandle(single);
        return 0;
    }

    // created once, kept open for the lifetime of the process so that
    // the setup program can always find and signal it
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, ETS2FAST_STOP);
    if (stop) ResetEvent(stop);

    std::wstring dll    = g_dir + L"ets2fast.dll";
    std::wstring dllLog = g_dir + L"ets2fast.log";

    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        say(L"daemon: ets2fast.dll is missing next to this exe, giving up");
        if (stop) CloseHandle(stop);
        if (single) CloseHandle(single);
        return 2;
    }

    say(L"daemon: started, watching for eurotrucks2.exe");

    DWORD injectedPid = 0;
    int   attempts    = 0;

    for (;;) {
        DWORD w = stop ? WaitForSingleObject(stop, 250) : (Sleep(250), WAIT_TIMEOUT);
        if (w == WAIT_OBJECT_0) {
            say(L"daemon: stop requested, exiting");
            break;
        }

        DWORD pid = find_proc(L"eurotrucks2.exe");

        if (!pid) {
            if (injectedPid) say(L"daemon: game closed, ready for the next launch");
            injectedPid = 0;
            attempts    = 0;
            continue;
        }
        if (pid == injectedPid) continue;
        if (attempts >= 3) continue;              // do not hammer a process that refuses

        ++attempts;
        long long mark = file_size(dllLog);
        if (inject_retry(pid, dll, 6)) {
            injectedPid = pid;
            say(L"daemon: injected into pid %lu", pid);
            say(hook_is_live(dllLog, mark, 10) ? L"daemon: hook is live"
                                               : L"daemon: injected but unconfirmed, see ets2fast.log");
        } else {
            say(L"daemon: injection attempt %d failed for pid %lu", attempts, pid);
        }
    }

    if (stop) CloseHandle(stop);
    if (single) CloseHandle(single);
    return 0;
}

static int run()
{
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    g_dir = self;
    size_t slash = g_dir.find_last_of(L'\\');
    g_dir = (slash == std::wstring::npos) ? std::wstring() : g_dir.substr(0, slash + 1);
    g_logPath = g_dir + L"ets2fast-launcher.log";
    g_console = (GetConsoleWindow() != nullptr);

    // parse: [own exe] [--launch <game exe> [args...]] | [--daemon]
    const wchar_t* p = skip_ws(GetCommandLineW());
    p = skip_token(p);
    p = skip_ws(p);

    if (wcsncmp(p, L"--launch", 8) == 0) {
        p = skip_ws(p + 8);
        const wchar_t* b = p;
        const wchar_t* e = skip_token(p);
        std::wstring gameExe = token_str(b, e);
        std::wstring gameArgs = skip_ws(e);
        if (gameExe.empty()) {
            say(L"--launch given without a game path");
            return 2;
        }
        return launch_mode(gameExe, gameArgs);
    }

    if (wcsncmp(p, L"--daemon", 8) == 0) return daemon_mode();

    // no switch: the windowless build is the daemon, the console build waits once
#ifdef ETS2FAST_DAEMON_DEFAULT
    return daemon_mode();
#else
    return watch_mode();
#endif
}

// console build (inject.exe) and windowless build (ets2fastd.exe) share this file
#ifdef ETS2FAST_GUI
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) { return run(); }
#else
int wmain() { return run(); }
#endif
