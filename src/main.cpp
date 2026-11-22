// ets2fast.dll - hook NtCreateFile, answer "does this mod path exist" from RAM
// build: g++ -shared -O2 -s -o ets2fast.dll main.cpp -static-libgcc -static-libstdc++ -Wl,--kill-at
//
// ETS2 resolves resources by probing every mod layer; ~98% of those probes hit
// paths that do not exist. We keep an in-memory index of the workshop tree and
// return STATUS_OBJECT_PATH_NOT_FOUND immediately when a path cannot exist.

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <string_view>
#include <unordered_set>
#include <cwctype>
#include <cstring>
#include <cstdio>
#include <cstdarg>

typedef LONG NTSTATUS;
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xC000003AL)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)

typedef struct { USHORT Len; USHORT MaxLen; PWSTR Buf; } USTR;
typedef struct { ULONG Length; HANDLE Root; USTR* Name; ULONG Attr; PVOID Sd; PVOID Sqos; } OBJATTR;
typedef struct { union { NTSTATUS St; PVOID P; }; ULONG_PTR Info; } IOSB;

typedef NTSTATUS (NTAPI *FN_NtCreateFile)(
    PHANDLE, ACCESS_MASK, OBJATTR*, IOSB*, PLARGE_INTEGER,
    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

// ---- hash set with transparent lookup (no allocation per probe) ----
struct WHash {
    using is_transparent = void;
    size_t operator()(std::wstring_view v) const noexcept {
        size_t h = 1469598103934665603ULL;
        for (wchar_t c : v) { h ^= (size_t)c; h *= 1099511628211ULL; }
        return h;
    }
    size_t operator()(const std::wstring& s) const noexcept { return (*this)(std::wstring_view(s)); }
};
struct WEq {
    using is_transparent = void;
    bool operator()(std::wstring_view a, std::wstring_view b) const noexcept { return a == b; }
    bool operator()(const std::wstring& a, const std::wstring& b) const noexcept { return a == b; }
};
typedef std::unordered_set<std::wstring, WHash, WEq> Index;
static Index g_index;

static FN_NtCreateFile g_real = nullptr;
static BYTE*  g_tramp    = nullptr;
static BYTE   g_saved[16];
static SIZE_T g_patchLen = 0;

static volatile LONG g_ready = 0;
static volatile LONGLONG g_hit  = 0;   // blocked (proven absent)
static volatile LONGLONG g_pass = 0;   // forwarded

static wchar_t g_logPath[MAX_PATH * 2] = {0};
static int     g_debug = 0;   // ETS2FAST_DEBUG=1 -> log every blocked path
static wchar_t g_root[MAX_PATH * 2] = {0};       // workshop\227300\ that got indexed
static thread_local std::wstring g_scratch;

// guards g_index: readers take it shared on every probe, the rebuild takes it
// exclusive. Cheap enough (tens of ns) compared to the syscall it replaces.
static SRWLOCK g_lock = SRWLOCK_INIT;

struct SharedLock {
    SharedLock()  { AcquireSRWLockShared(&g_lock); }
    ~SharedLock() { ReleaseSRWLockShared(&g_lock); }
};

// ---------------- logging ----------------
static void logf(const char* fmt, ...)
{
    if (!g_logPath[0]) return;
    FILE* f = _wfopen(g_logPath, L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void logw(const char* tag, const wchar_t* w)
{
    char mb[2048];
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, mb, sizeof(mb) - 1, nullptr, nullptr);
    if (n <= 0) return;
    mb[n] = 0;
    logf("%s %s", tag, mb);
}

// ---------------- helpers ----------------
static inline wchar_t lc(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c + 32);   // locale independent
    if (c < 0x80) return c;
    return (wchar_t)towlower(c);                            // non-ASCII: best effort
}

static void to_lower_inplace(std::wstring& s) {
    for (auto& c : s) c = lc(c);
}

// case-sensitive ASCII token search inside a wide buffer
static bool wcontains(const wchar_t* b, size_t n, const wchar_t* tok, size_t tn)
{
    if (n < tn) return false;
    for (size_t i = 0; i + tn <= n; ++i) {
        size_t j = 0;
        while (j < tn && b[i + j] == tok[j]) ++j;
        if (j == tn) return true;
    }
    return false;
}

// ---------------- index ----------------
// key = "<modid>\<relative path>", lowercase, no trailing backslash
static void scan_dir(const std::wstring& base, const std::wstring& cur, int depth)
{
    if (depth > 24) return;

    std::wstring pattern = cur + L"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;

        std::wstring child = cur + fd.cFileName;          // no trailing backslash
        std::wstring rel   = child.substr(base.size());   // relative to workshop\227300 (trailing backslash removed)
        if (!rel.empty()) {
            g_index.insert(rel);                          // original case
            std::wstring low = rel;
            to_lower_inplace(low);
            if (low != rel) g_index.insert(low);           // plus lowercase form
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            scan_dir(base, child + L"\\", depth + 1);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

// index the given root; false if the directory does not exist
static bool index_root(const std::wstring& rootIn, const char* how)
{
    std::wstring d = rootIn;
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    if (GetFileAttributesW(d.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    {
        AcquireSRWLockExclusive(&g_lock);
        g_index.clear();
        scan_dir(d, d, 0);
        ReleaseSRWLockExclusive(&g_lock);
    }
    wcscpy(g_root, d.c_str());
    logf("root (%s): %ls", how, d.c_str());
    logf("index entries: %zu", g_index.size());
    return true;
}

// the game exe sits at <steamapps>\common\<game>\bin\win_x64\, so the workshop
// location can be derived from wherever the game is installed - no config needed
static bool derive_root_from_exe(std::wstring& out)
{
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return false;

    std::wstring p = exe;
    for (int guard = 0; guard < 14; ++guard) {
        size_t slash = p.rfind(L'\\');
        if (slash == std::wstring::npos || slash < 3) return false;
        p.resize(slash);
        size_t s2 = p.rfind(L'\\');
        std::wstring leaf = (s2 == std::wstring::npos) ? p : p.substr(s2 + 1);
        if (_wcsicmp(leaf.c_str(), L"steamapps") == 0) {
            out = p + L"\\workshop\\content\\227300";
            return true;
        }
    }
    return false;
}

static bool build_index()
{
    // 1) explicit override, used by tests and unusual layouts
    wchar_t envRoot[MAX_PATH * 2] = {0};
    if (GetEnvironmentVariableW(L"ETS2FAST_ROOT", envRoot, MAX_PATH * 2) > 0) {
        if (index_root(envRoot, "ETS2FAST_ROOT")) return true;
        logf("ETS2FAST_ROOT unusable: %ls", envRoot);
    }

    // 2) derived from the game's own location - portable, zero configuration
    std::wstring derived;
    if (derive_root_from_exe(derived)) {
        logf("derived workshop path: %ls", derived.c_str());
        if (index_root(derived, "derived from game path")) return true;
    }

    // 3) last resort: the usual default locations
    const wchar_t* roots[] = {
        L"G:\\steam\\steamapps\\workshop\\content\\227300",
        L"C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\227300",
        L"D:\\SteamLibrary\\steamapps\\workshop\\content\\227300",
        L"E:\\SteamLibrary\\steamapps\\workshop\\content\\227300",
        L"F:\\SteamLibrary\\steamapps\\workshop\\content\\227300",
    };
    for (auto r : roots) {
        if (index_root(r, "known location")) return true;
    }

    logf("root: NOT FOUND - no hook installed");
    return false;
}

// ---------------- decision ----------------
// true only when the path PROVABLY cannot exist.
// Walks component by component: once one component is missing from the index,
// every deeper component is missing too. If the very first component (the mod
// id) is unknown we stay out of the way.
static NTSTATUS path_status(const wchar_t* buf, USHORT byteLen, std::wstring* failAt = nullptr)
{
    if (!buf) return 0;
    size_t n = byteLen / sizeof(wchar_t);
    if (n < 8) return 0;

    // cheap reject: most IO in the process has nothing to do with workshop mods
    if (!wcontains(buf, n, L"227300", 6)) return 0;

    // require an absolute path
    size_t off = 0;
    if (n >= 4 && buf[0] == L'\\' && buf[1] == L'?' && buf[2] == L'?' && buf[3] == L'\\')      off = 4;
    else if (n >= 4 && buf[0] == L'\\' && buf[1] == L'\\' && buf[2] == L'?' && buf[3] == L'\\') off = 4;
    else if (!(n >= 3 && buf[1] == L':')) return 0;

    size_t pos = (size_t)-1;
    for (size_t i = off; i + 8 <= n; ++i) {
        if (buf[i] == L'\\' && buf[i + 1] == L'2' && buf[i + 2] == L'2' &&
            buf[i + 3] == L'7' && buf[i + 4] == L'3' && buf[i + 5] == L'0' &&
            buf[i + 6] == L'0' && buf[i + 7] == L'\\') { pos = i; break; }
    }
    if (pos == (size_t)-1) return 0;

    size_t s = pos + 8, e = n;
    while (e > s && buf[e - 1] == L'\\') --e;
    if (e <= s) return 0;

    // lowercase once into a reused buffer
    g_scratch.assign(buf + s, e - s);
    for (auto& c : g_scratch) c = lc(c);

    SharedLock guard;

    const size_t len = g_scratch.size();
    bool ancestor_known = false;
    size_t seg = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || g_scratch[i] == L'\\') {
            if (i == seg) return 0;                       // empty component, stay out
            if (g_index.find(std::wstring_view(g_scratch.data(), i)) == g_index.end()) {
                if (failAt) failAt->assign(g_scratch, 0, i);
                if (!ancestor_known) return 0;             // unknown root: stay out
                // mirror what the filesystem itself would report
                return (i == len) ? STATUS_OBJECT_NAME_NOT_FOUND     // only the leaf is absent
                                  : STATUS_OBJECT_PATH_NOT_FOUND;    // a parent is absent
            }
            ancestor_known = true;
            seg = i + 1;
        }
    }
    return 0;                                             // fully present
}

// ---------------- inline hook ----------------
static void suspend_others(HANDLE* out, int& cnt)
{
    cnt = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD self = GetCurrentThreadId(), me = GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == me && te.th32ThreadID != self && cnt < 512) {
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (th) { SuspendThread(th); out[cnt++] = th; }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void resume_all(HANDLE* h, int cnt)
{
    for (int i = 0; i < cnt; ++i) { ResumeThread(h[i]); CloseHandle(h[i]); }
}

// allocate a page within +-2GB of 'hint' so a rel32 jump can reach it
static BYTE* alloc_near(void* hint)
{
    ULONG_PTR base = (ULONG_PTR)hint & ~(ULONG_PTR)0xFFFF;
    for (ULONG_PTR delta = 0x10000; delta < 0x70000000ULL; delta += 0x10000) {
        BYTE* m = (BYTE*)VirtualAlloc((void*)(base - delta), 0x1000,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (m) return m;
        m = (BYTE*)VirtualAlloc((void*)(base + delta), 0x1000,
                                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (m) return m;
    }
    return nullptr;
}

// "jmp [rip+0]; qword dest" (14 bytes)
static void write_abs_jump(BYTE* at, void* dest)
{
    at[0] = 0xFF;
    at[1] = 0x25;
    *(DWORD*)(at + 2) = 0;
    *(void**)(at + 6) = dest;
}

static bool patch(void* target, void* detour)
{
    BYTE* p = (BYTE*)target;

    logf("prologue: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);

    // ---- form 1: Windows 11 export thunk  "48 B8 <imm64> FF E0" (mov rax,X ; jmp rax)
    // only the 8-byte immediate changes: no code shifting, no trampoline
    if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) {
        void* impl = nullptr;
        memcpy(&impl, p + 2, 8);
        logf("thunk form detected, real impl = %p", impl);
        if (!impl) return false;

        DWORD old;
        if (!VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old)) { logf("thunk: VirtualProtect failed"); return false; }

        HANDLE th[512];
        int cnt = 0;
        suspend_others(th, cnt);
        memcpy(p + 2, &detour, 8);
        FlushInstructionCache(GetCurrentProcess(), p, 16);
        resume_all(th, cnt);

        VirtualProtect(p, 16, old, &old);
        g_real = (FN_NtCreateFile)impl;            // call the real body directly
        logf("thunk patched, suspended threads: %d", cnt);
        return true;
    }

    // ---- form 2: classic syscall stub "4C 8B D1 ; B8 ssn"
    SIZE_T need = 0;
    while (need < 5) {
        BYTE op = p[need];
        if (op == 0x4C && p[need + 1] == 0x8B)      need += 3;
        else if (op == 0xB8)                        need += 5;
        else if (op == 0x48 && p[need + 1] == 0x83) need += 4;
        else if (op == 0x48 && p[need + 1] == 0x8B) need += 3;
        else { logf("unknown prologue, giving up"); return false; }
    }
    if (need > sizeof(g_saved)) return false;

    memcpy(g_saved, p, need);
    g_patchLen = need;

    g_tramp = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp) return false;

    memcpy(g_tramp, g_saved, need);
    write_abs_jump(g_tramp + need, p + need);
    FlushInstructionCache(GetCurrentProcess(), g_tramp, need + 14);

    g_real = (FN_NtCreateFile)g_tramp;

    LONGLONG rel = (LONGLONG)((BYTE*)detour - (p + 5));
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) {
        BYTE* relay = alloc_near(target);
        if (!relay) return false;
        write_abs_jump(relay, detour);
        rel = (LONGLONG)((BYTE*)relay - (p + 5));
        logf("relay stub used");
    }

    DWORD old;
    if (!VirtualProtect(p, need, PAGE_EXECUTE_READWRITE, &old)) return false;

    HANDLE th[512];
    int cnt = 0;
    suspend_others(th, cnt);
    p[0] = 0xE9;
    *(DWORD*)(p + 1) = (DWORD)(INT32)rel;
    for (SIZE_T i = 5; i < need; ++i) p[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, need);
    resume_all(th, cnt);

    VirtualProtect(p, need, old, &old);
    logf("stub patched (len=%zu), suspended threads: %d", need, cnt);
    return true;
}

static NTSTATUS NTAPI HookNtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess, OBJATTR* ObjectAttributes,
    IOSB* IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
    ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions,
    PVOID EaBuffer, ULONG EaLength)
{
    if (!g_real) return STATUS_OBJECT_PATH_NOT_FOUND;

    // only pure existence probing; anything that may write goes straight through
    bool writeish = (DesiredAccess & (0x40000000u | 0x2u | 0x4u | 0x10u | 0x100u | 0x10000u)) != 0;
    bool openish  = (CreateDisposition == 1 || CreateDisposition == 3);

    if (!writeish && openish
        && g_ready == 1
        && ObjectAttributes && ObjectAttributes->Name && ObjectAttributes->Name->Buf)
    {
        std::wstring failAt;
        NTSTATUS deny = path_status(ObjectAttributes->Name->Buf, ObjectAttributes->Name->Len,
                                    g_debug ? &failAt : nullptr);
        if (deny != 0) {
            if (g_debug) {
                static volatile LONG shown = 0;
                if (shown < 300) {
                    InterlockedIncrement(&shown);
                    wchar_t tmp[1024];
                    int c = ObjectAttributes->Name->Len / sizeof(wchar_t);
                    if (c > 1023) c = 1023;
                    memcpy(tmp, ObjectAttributes->Name->Buf, c * sizeof(wchar_t));
                    tmp[c] = 0;
                    logw("BLOCK", tmp);
                    logw("   MISSING", failAt.c_str());
                }
            }
            InterlockedIncrement64(&g_hit);
            if (IoStatusBlock) {
                IoStatusBlock->St   = deny;
                IoStatusBlock->Info = 0;
            }
            if (FileHandle) *FileHandle = nullptr;
            return deny;
        }
    }
    InterlockedIncrement64(&g_pass);

    return g_real(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
                  FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

// ---------------- live watch ----------------
// Mods can be added or removed while the game runs: subscribing inside the mod
// manager makes Steam download in the background, then "apply changes" reloads
// mods without restarting the process. While the tree is changing we stop
// intercepting entirely (vanilla behaviour), then rebuild and resume.
static bool watch_wait(HANDLE dir, OVERLAPPED* ov, BYTE* buf, DWORD bufSize,
                       DWORD timeoutMs, bool& changed)
{
    changed = false;
    ResetEvent(ov->hEvent);
    DWORD bytes = 0;

    if (!ReadDirectoryChangesW(dir, buf, bufSize, TRUE,
                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                               FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                               &bytes, ov, nullptr))
        return false;

    DWORD w = WaitForSingleObject(ov->hEvent, timeoutMs);
    if (w == WAIT_OBJECT_0) { changed = true; return true; }

    CancelIoEx(dir, ov);
    DWORD dummy = 0;
    GetOverlappedResult(dir, ov, &dummy, TRUE);
    return w == WAIT_TIMEOUT;
}

static DWORD WINAPI watch_thread(LPVOID)
{
    if (!g_root[0]) return 0;

    HANDLE dir = CreateFileW(g_root, FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        logf("watch: open failed %lu", GetLastError());
        return 1;
    }

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    static BYTE buf[0x8000];

    for (;;) {
        bool changed = false;
        // 60 s timeout doubles as a heartbeat: report counters while running,
        // because we deliberately write nothing at process detach
        if (!watch_wait(dir, &ov, buf, sizeof(buf), 60000, changed)) break;
        if (!changed) {
            logf("stats: blocked=%lld passed=%lld index=%zu",
                 (long long)g_hit, (long long)g_pass, g_index.size());
            continue;
        }

        // something under the workshop tree moved: fail open right away
        InterlockedExchange(&g_ready, 0);
        logf("watch: change detected -> interception paused");

        // keep waiting while more changes keep coming, rebuild after 5s of quiet
        while (watch_wait(dir, &ov, buf, sizeof(buf), 5000, changed) && changed) {}

        build_index();
        logf("watch: rebuilt (%zu entries) -> interception resumed", g_index.size());
        InterlockedExchange(&g_ready, 1);
    }

    CloseHandle(ov.hEvent);
    CloseHandle(dir);
    return 0;
}

// ---------------- init ----------------
// Falling back: if injection still beat the game's own initialisation, the
// process may be treated as DPI unaware, which breaks >100% scaling. Ask for
// per-monitor v2 - it fails harmlessly once a window already locked the mode.
static void try_restore_dpi()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    typedef BOOL (WINAPI* PFN_SET_CTX)(void*);
    PFN_SET_CTX fn = (PFN_SET_CTX)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (!fn) return;

    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo handle (void*)-4
    if (fn((void*)-4)) logf("dpi: context set to per-monitor v2");
    else               logf("dpi: context already locked (err=%lu), left unchanged", GetLastError());
}

static DWORD WINAPI init_thread(LPVOID)
{
    if (InterlockedCompareExchange(&g_ready, -1, 0) != 0) return 0;   // already running

    logf("=== ets2fast attach ===");
    try_restore_dpi();
    {
        wchar_t dbg[8] = {0};
        if (GetEnvironmentVariableW(L"ETS2FAST_DEBUG", dbg, 8) > 0) g_debug = _wtoi(dbg);
    }

    if (!build_index() || g_index.empty()) {
        logf("index empty -> hook NOT installed (game behaves normally)");
        InterlockedExchange(&g_ready, 0);
        return 1;
    }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) { logf("ntdll not found"); return 1; }

    void* target = (void*)GetProcAddress(ntdll, "NtCreateFile");
    if (!target) { logf("NtCreateFile not found"); return 1; }

    if (!patch(target, (void*)HookNtCreateFile)) {
        logf("hook NOT installed");
        return 1;
    }

    logf("hook installed");
    InterlockedExchange(&g_ready, 1);

    // keep the index in sync with mods added/removed while the game is running
    HANDLE w = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
    if (w) CloseHandle(w);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameW(hModule, g_logPath, MAX_PATH);
        wchar_t* s = wcsrchr(g_logPath, L'\\');
        if (s) wcscpy(s + 1, L"ets2fast.log");
        else   g_logPath[0] = 0;

        HANDLE h = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        // nothing here on purpose: at detach the CRT may already be torn down,
        // and any fopen/printf from this point can fail-fast the host process.
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int Ets2FastDumpSub(const wchar_t* sub)
{
    SharedLock guard;
    int n = 0;
    for (auto& k : g_index) {
        if (k.find(sub) == std::wstring::npos) continue;
        logw("IDX", k.c_str());
        if (++n >= 60) break;
    }
    return n;
}

extern "C" __declspec(dllexport) BOOL Ets2FastReady()     { return g_ready == 1; }
extern "C" __declspec(dllexport) LONGLONG Ets2FastHits()  { return g_hit; }
