// ets2fast.exe - console menu
//
//   [1] enable auto inject  : start the background daemon + register it to run
//                             at logon (HKEY_CURRENT_USER, no admin needed)
//   [2] disable auto inject : unregister + stop the daemon, back to running
//                             inject.exe by hand
//
// Nothing is written outside HKEY_CURRENT_USER and this program's own folder,
// so it works the same on any Windows machine without elevation.

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string>
#include <vector>

#define RUN_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_NAME   L"Ets2FastDaemon"
#define DAEMON_EXE L"ets2fastd.exe"
#define DLL_NAME   L"ets2fast.dll"

static std::wstring g_dir;

// self-healing state, shared with the action handlers further down
static bool  g_healChecked = false;
static bool  g_healDone    = false;
static DWORD g_healPid     = 0;

// ---------------- console output ----------------
static HANDLE g_out = INVALID_HANDLE_VALUE;
static WORD   g_attr = 7;

static void out(const wchar_t* s)
{
    DWORD n = 0;
    if (!WriteConsoleW(g_out, s, (DWORD)wcslen(s), &n, nullptr)) {
        // redirected to a file or a pipe: fall back to the ANSI code page
        char mb[4096];
        int k = WideCharToMultiByte(CP_ACP, 0, s, -1, mb, sizeof(mb) - 1, nullptr, nullptr);
        if (k > 0) { mb[k] = 0; fputs(mb, stdout); }
    }
}

static void outln(const wchar_t* s) { out(s); out(L"\n"); }

static void color(WORD c) { SetConsoleTextAttribute(g_out, c); }
static void reset_color() { SetConsoleTextAttribute(g_out, g_attr); }

// returns false on end of input, so a closed stdin cannot spin the menu forever
static bool read_line(std::wstring& result)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;

    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        wchar_t buf[256];
        DWORD n = 0;
        ZeroMemory(buf, sizeof(buf));
        if (!ReadConsoleW(in, buf, 200, &n, nullptr) || n == 0) return false;
        while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ')) buf[--n] = 0;
        result.assign(buf, n);
        return true;
    }

    // redirected input (pipes, scripts)
    wchar_t buf[256];
    if (!fgetws(buf, 250, stdin)) return false;
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    result = s;
    return true;
}

static void pause_enter()
{
    out(L"\n按回车返回菜单...");
    std::wstring tmp;
    read_line(tmp);
}

// ---------------- process helpers ----------------
// A single toolhelp snapshot can come back short on a busy system, which would
// make a perfectly healthy daemon look dead. Retry before believing "not running".
static DWORD find_proc(const wchar_t* name)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            Sleep(30);
            continue;
        }

        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;
        BOOL ok = Process32FirstW(snap, &pe);
        while (ok) {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
            ok = Process32NextW(snap, &pe);
        }
        CloseHandle(snap);

        if (pid) return pid;

        // the walk ended for a reason other than "no more entries" -> unreliable
        if (ok == FALSE && GetLastError() != ERROR_NO_MORE_FILES) {
            Sleep(30);
            continue;
        }
        return 0;                   // walked everything, genuinely not running
    }
    return 0;
}

static ULONGLONG filetime_now()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER li;
    li.LowPart  = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    return li.QuadPart;
}

// seconds the process has been alive, 0 if it cannot be queried
static ULONGLONG proc_uptime_seconds(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;

    FILETIME created, exited, kernel, user;
    ULONGLONG started = 0;
    if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER li;
        li.LowPart  = created.dwLowDateTime;
        li.HighPart = created.dwHighDateTime;
        started = li.QuadPart;
    }
    CloseHandle(h);

    if (!started) return 0;
    ULONGLONG now = filetime_now();
    if (now <= started) return 0;
    return (now - started) / 10000000ULL;
}

static bool exists(const std::wstring& p)
{
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---------------- autostart (HKEY_CURRENT_USER) ----------------
static std::wstring autostart_read()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return L"";
    wchar_t buf[4096];
    DWORD cb = sizeof(buf), type = 0;
    std::wstring v;
    if (RegQueryValueExW(k, RUN_NAME, nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS)
        v.assign(buf);
    RegCloseKey(k);
    return v;
}

static bool autostart_write(const std::wstring& value)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(k, RUN_NAME, 0, REG_SZ,
                            (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static bool autostart_remove()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return true;
    LONG r = RegDeleteValueW(k, RUN_NAME);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
}

// the command line stored in the registry, quoted because the path may contain spaces
static std::wstring autostart_command()
{
    return L"\"" + g_dir + DAEMON_EXE + L"\"";
}

// ---------------- daemon control ----------------
static bool start_daemon()
{
    std::wstring exe = g_dir + DAEMON_EXE;
    if (!exists(exe)) return false;
    if (find_proc(DAEMON_EXE)) return true;

    // Go through the shell first. The replacement is then created by explorer,
    // which does not belong to whatever job this program may be running in.
    // A job with KILL_ON_JOB_CLOSE is exactly what makes background processes
    // disappear silently when their starter exits.
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize      = sizeof(sei);
    sei.fMask       = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb      = L"open";
    sei.lpFile      = exe.c_str();
    sei.lpDirectory = g_dir.c_str();
    sei.nShow       = SW_HIDE;

    if (ShellExecuteExW(&sei) && sei.hProcess) {
        CloseHandle(sei.hProcess);
    } else {
        std::wstring cmd = L"\"" + exe + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
        BOOL started = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                                      flags, nullptr, g_dir.c_str(), &si, &pi);
        if (!started) {
            flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
            started = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                                     flags, nullptr, g_dir.c_str(), &si, &pi);
        }
        if (!started) return false;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    // give it a moment to come up, so the caller can report the truth
    for (int i = 0; i < 40; ++i) {
        if (find_proc(DAEMON_EXE)) break;
        Sleep(50);
    }
    return find_proc(DAEMON_EXE) != 0;
}

static bool stop_daemon()
{
    DWORD pid = find_proc(DAEMON_EXE);
    if (!pid) return true;

    // ask politely first: the daemon watches this event and exits on its own
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\ets2fast_daemon_stop");
    if (ev) {
        SetEvent(ev);
        CloseHandle(ev);
        for (int i = 0; i < 40; ++i) {
            if (!find_proc(DAEMON_EXE)) return true;
            Sleep(100);
        }
    }

    // fall back to terminating it
    pid = find_proc(DAEMON_EXE);
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (h) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
        }
    }
    for (int i = 0; i < 20; ++i) {
        if (!find_proc(DAEMON_EXE)) return true;
        Sleep(100);
    }
    return find_proc(DAEMON_EXE) == 0;
}

// ---------------- steam launch option leftovers ----------------
static std::wstring steam_dir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD cb = sizeof(buf), type = 0;

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        cb = sizeof(buf);
        if (RegQueryValueExW(k, L"SteamPath", nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS) {
            RegCloseKey(k);
            std::wstring s = buf;
            for (size_t i = 0; i < s.size(); ++i) if (s[i] == L'/') s[i] = L'\\';
            return s;
        }
        RegCloseKey(k);
    }

    const wchar_t* guesses[] = {
        L"C:\\Program Files (x86)\\Steam",
        L"C:\\Program Files\\Steam",
        L"D:\\Steam",
        L"E:\\Steam",
        L"F:\\Steam",
    };
    for (int i = 0; i < 5; ++i) if (exists(guesses[i])) return guesses[i];
    return L"";
}

static bool file_has_text(const std::wstring& path, const char* needle)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (32LL << 20)) {
        CloseHandle(h);
        return false;
    }

    std::vector<char> data((size_t)sz.QuadPart + 1);
    DWORD got = 0;
    ReadFile(h, data.data(), (DWORD)sz.QuadPart, &got, nullptr);
    CloseHandle(h);
    data[got] = 0;

    // case insensitive ASCII compare is enough here: we look for our own file name
    for (size_t i = 0; i < data.size(); ++i) if (data[i] >= 'A' && data[i] <= 'Z') data[i] = (char)(data[i] + 32);
    return strstr(data.data(), needle) != nullptr;
}

static bool steam_launchopt_has_ets2fast()
{
    std::wstring sd = steam_dir();
    if (sd.empty()) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((sd + L"\\userdata\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        std::wstring vdf = sd + L"\\userdata\\" + fd.cFileName + L"\\config\\localconfig.vdf";
        if (file_has_text(vdf, "ets2fast")) { found = true; break; }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

// ---------------- status ----------------
static void print_status()
{
    DWORD daemonPid   = find_proc(DAEMON_EXE);
    bool  daemonUp    = daemonPid != 0;
    std::wstring v    = autostart_read();
    bool  autostartSet = (v.find(L"ets2fast") != std::wstring::npos ||
                          v.find(L"Ets2Fast") != std::wstring::npos);

    out(L"  ");
    color(8);
    out(L"自动注入: ");
    reset_color();

    if (daemonUp && autostartSet) {
        color(10);
        outln(L"已开启");
        reset_color();

        // the uptime tells you whether the daemon has been restarted since
        // you last looked, which is exactly what matters when it "disappears"
        ULONGLONG up = proc_uptime_seconds(daemonPid);
        wchar_t line[200];
        if (up >= 3600)
            _snwprintf(line, 200, L"            后台守护 PID %lu，已运行 %llu 小时 %llu 分，开机自动启动",
                       daemonPid, up / 3600, (up % 3600) / 60);
        else if (up >= 60)
            _snwprintf(line, 200, L"            后台守护 PID %lu，已运行 %llu 分 %llu 秒，开机自动启动",
                       daemonPid, up / 60, up % 60);
        else if (up > 0)
            _snwprintf(line, 200, L"            后台守护 PID %lu，刚启动 %llu 秒，开机自动启动",
                       daemonPid, up);
        else
            _snwprintf(line, 200, L"            后台守护 PID %lu，刚刚启动，开机自动启动", daemonPid);
        line[199] = 0;
        outln(line);
    } else if (daemonUp && !autostartSet) {
        color(14);
        outln(L"部分开启（守护在跑，但没有开机自启）");
        reset_color();
        out(L"            后台守护 PID ");
        wchar_t pidbuf[32];
        _snwprintf(pidbuf, 32, L"%lu", daemonPid);
        pidbuf[31] = 0;
        outln(pidbuf);
        outln(L"            按 1 补上开机自启");
    } else if (!daemonUp && autostartSet) {
        color(12);
        outln(L"异常：已配置开机自启，但后台守护没有运行");
        reset_color();
        outln(L"            按 1 重新拉起，或直接双击 " DAEMON_EXE);
    } else {
        color(7);
        outln(L"未开启（手动方式）");
        reset_color();
        outln(L"            玩之前先双击 " DAEMON_EXE L" 或 inject.exe");
    }

    // warn when the registry points at a folder the program no longer lives in
    if (autostartSet) {
        std::wstring want = autostart_command();
        if (_wcsicmp(v.c_str(), want.c_str()) != 0) {
            color(12);
            outln(L"\n  ! 开机自启里记录的是旧路径，程序被移动过");
            reset_color();
            out(L"    记录值: "); outln(v.c_str());
            out(L"    当前值: "); outln(want.c_str());
        }
    }

    if (find_proc(L"eurotrucks2.exe")) {
        color(11);
        outln(L"\n  * 游戏正在运行中，本次改动要到下次启动游戏才生效");
        reset_color();
    }

    if (!exists(g_dir + DLL_NAME) || !exists(g_dir + DAEMON_EXE)) {
        color(12);
        outln(L"\n  ! 目录里缺少 " DLL_NAME L" 或 " DAEMON_EXE);
        reset_color();
    }
}

static void show_log()
{
    out(L"\n----- ets2fast.log 末尾 -----\n");
    std::wstring p = g_dir + L"ets2fast.log";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        outln(L"(还没有日志，跑一次游戏再看)");
        return;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    long long from = sz.QuadPart > 3000 ? sz.QuadPart - 3000 : 0;
    LARGE_INTEGER pos;
    pos.QuadPart = from;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);

    char buf[3200];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    buf[got] = 0;

    wchar_t wbuf[3600];
    int k = MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 3500);
    if (k <= 0) k = MultiByteToWideChar(CP_ACP, 0, buf, -1, wbuf, 3500);
    if (k > 0) outln(wbuf);
    else outln(L"(日志无法解码)");

    out(L"\n----- ets2fast-launcher.log 末尾 -----\n");
    std::wstring p2 = g_dir + L"ets2fast-launcher.log";
    HANDLE h2 = CreateFileW(p2.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h2 == INVALID_HANDLE_VALUE) { outln(L"(无)"); return; }
    LARGE_INTEGER sz2;
    GetFileSizeEx(h2, &sz2);
    long long from2 = sz2.QuadPart > 2000 ? sz2.QuadPart - 2000 : 0;
    pos.QuadPart = from2;
    SetFilePointerEx(h2, pos, nullptr, FILE_BEGIN);
    DWORD got2 = 0;
    ReadFile(h2, buf, sizeof(buf) - 1, &got2, nullptr);
    CloseHandle(h2);
    buf[got2] = 0;
    k = MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 3500);
    if (k > 0) outln(wbuf); else outln(L"(无法解码)");
}

// ---------------- actions ----------------
static void do_enable()
{
    g_healDone = false;          // the notice is stale once the user acts
    out(L"\n");

    if (!exists(g_dir + DLL_NAME)) {
        color(12);
        outln(L"  [X] 找不到 " DLL_NAME);
        reset_color();
        outln(L"      请把本程序和 " DLL_NAME L" 放在同一个文件夹里");
        pause_enter();
        return;
    }
    if (!exists(g_dir + DAEMON_EXE)) {
        color(12);
        outln(L"  [X] 找不到 " DAEMON_EXE);
        reset_color();
        pause_enter();
        return;
    }

    std::wstring cmd = autostart_command();
    if (!autostart_write(cmd)) {
        color(12);
        outln(L"  [X] 写入开机自启失败（注册表拒绝访问）");
        reset_color();
        pause_enter();
        return;
    }
    out(L"  [1/2] 已写入开机自启\n");

    bool up = start_daemon();
    out(L"  [2/2] 后台守护: ");
    color(up ? 10 : 12);
    outln(up ? L"已启动" : L"启动失败");
    reset_color();

    if (!up) {
        outln(L"        直接双击 " DAEMON_EXE L" 看看有没有报错");
        pause_enter();
        return;
    }

    color(10);
    outln(L"\n  完成。现在从 Steam 点开始游戏就会自动注入，不用再开任何程序。");
    reset_color();

    if (steam_launchopt_has_ets2fast()) {
        color(14);
        outln(L"\n  另外发现: Steam 启动选项里还留着旧的 ets2fast 配置。");
        reset_color();
        outln(L"  建议清掉，避免同时用两种方式:");
        outln(L"    Steam 库 -> 右键 Euro Truck Simulator 2 -> 属性 -> 通用 -> 启动选项");
        outln(L"    删除里面含 ets2fast 的那一段，只保留 -nointro 等参数");
    }

    if (find_proc(L"eurotrucks2.exe")) {
        color(11);
        outln(L"\n  游戏正在运行，下次启动游戏时才会注入。");
        reset_color();
    }
    pause_enter();
}

static void do_disable()
{
    g_healDone = false;
    out(L"\n");

    bool regOk = autostart_remove();
    out(L"  [1/2] 开机自启: ");
    color(regOk ? 10 : 12);
    outln(regOk ? L"已移除" : L"移除失败");
    reset_color();

    bool stopped = stop_daemon();
    out(L"  [2/2] 后台守护: ");
    color(stopped ? 10 : 12);
    outln(stopped ? L"已停止" : L"仍在运行，请手动结束 " DAEMON_EXE);
    reset_color();

    color(10);
    outln(L"\n  已关闭自动注入。");
    reset_color();
    outln(L"  以后每次玩之前: 先双击 inject.exe，再从 Steam 启动游戏。");
    pause_enter();
}

// ---------------- compatibility: old Steam launch option ----------------
// If an earlier setup still has  "...\ets2fast.exe" --launch %command%  in the
// Steam launch options, Steam calls us with --launch. Forward it to the daemon
// binary, which knows how to wrap the game, and pass the exit code back so that
// Steam keeps tracking the game correctly.
static int forward_launch()
{
    static const wchar_t* k = L"--launch";
    const wchar_t* p = GetCommandLineW();
    const wchar_t* hit = wcsstr(p, k);
    if (!hit) return -1;

    std::wstring rest = hit + wcslen(k);
    size_t b = rest.find_first_not_of(L" \t");
    rest = (b == std::wstring::npos) ? std::wstring() : rest.substr(b);
    if (rest.empty()) return -1;

    std::wstring exe = g_dir + DAEMON_EXE;
    if (!exists(exe)) return -1;

    std::wstring cmd = L"\"" + exe + L"\" " + k + L" " + rest;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                        0, nullptr, g_dir.c_str(), &si, &pi))
        return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// ---------------- self healing ----------------
// Auto inject is configured, but the daemon is gone. That can happen for
// reasons outside our control: a job object closing, a "cleanup" utility,
// Task Manager, a crash. Nothing is watching it, so we do it here: every time
// the menu opens, a missing daemon gets started again.
static void self_heal()
{
    if (g_healChecked) return;
    g_healChecked = true;

    std::wstring v = autostart_read();
    bool configured = (v.find(L"ets2fast") != std::wstring::npos ||
                       v.find(L"Ets2Fast") != std::wstring::npos);
    if (!configured) return;            // auto inject is off on purpose

    if (find_proc(DAEMON_EXE)) return;  // healthy

    if (start_daemon()) {
        g_healDone = true;
        g_healPid  = find_proc(DAEMON_EXE);
    }
}

static void draw_menu()
{
    out(L"\n");
    color(11);
    out(L"  ==========================================================\n");
    out(L"     ETS2 载入加速  -  设置           ");
    color(8);
    out(L"ets2fast\n");
    color(11);
    out(L"  ==========================================================\n");
    reset_color();

    if (g_healDone) {
        wchar_t line[160];
        _snwprintf(line, 160, L"  * 后台守护之前已停止，已自动重新启动（PID %lu）", g_healPid);
        line[159] = 0;
        color(10);
        outln(line);
        reset_color();
        outln(L"    日志里最后一条 daemon: 记录能看出它是什么时候停的");
        out(L"\n");
    }

    print_status();

    out(L"\n");
    color(15);
    out(L"  [1] ");
    reset_color();
    outln(L"开启自动注入（后台守护 + 开机自启）");

    color(15);
    out(L"  [2] ");
    reset_color();
    outln(L"关闭自动注入（改回先开 inject.exe）");

    color(15);
    out(L"  [3] ");
    reset_color();
    outln(L"查看状态与日志");

    color(15);
    out(L"  [0] ");
    reset_color();
    outln(L"退出");

    out(L"\n  请选择后按回车: ");
}

int wmain()
{
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    g_dir = self;
    size_t slash = g_dir.find_last_of(L'\\');
    g_dir = (slash == std::wstring::npos) ? std::wstring() : g_dir.substr(0, slash + 1);

    // old Steam launch option points here: behave as a launcher, no menu
    {
        int rc = forward_launch();
        if (rc >= 0) return rc;
    }

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_out == INVALID_HANDLE_VALUE || g_out == nullptr) return 1;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_out, &csbi)) g_attr = csbi.wAttributes;

    SetConsoleTitleW(L"ETS2 载入加速 - 设置");

    // before the first screen: if auto inject is on but the daemon is missing,
    // start it again rather than only telling the user something is wrong
    self_heal();

    for (;;) {
        draw_menu();
        std::wstring line;
        if (!read_line(line)) break;          // stdin closed
        if (line.empty()) continue;

        wchar_t c = line[0];
        if (c == L'1')      do_enable();
        else if (c == L'2') do_disable();
        else if (c == L'3') { out(L"\n"); print_status(); show_log(); pause_enter(); }
        else if (c == L'0') break;
        else {
            out(L"\n  只认 1 / 2 / 3 / 0，按回车重来");
            Sleep(700);
        }
    }

    reset_color();
    return 0;
}
