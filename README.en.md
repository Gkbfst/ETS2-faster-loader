# ets2fast

**Load-time accelerator for Euro Truck Simulator 2** — cuts save loading from over a minute to under thirty seconds, without touching game files or any mod.

[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078D6)](#)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

[简体中文](README.md) | **English**

---

## Results

Same machine, same 58 workshop mods, same load, measured with and without the tool:

| Stage | Without | With | Delta |
|---|---:|---:|---:|
| Main menu appears | 8.42 s | 8.93 s | — |
| Mod enumeration (58) | 25.05 s | 23.27 s | −1.8 s |
| Mounting 58 packages | 0.33 s | 0.35 s | ≈0 |
| Traffic / cities / prefabs | 6.86 s | 6.37 s | −0.5 s |
| **Company cargo assignment** | **38.13 s** | **15.20 s** | **−22.9 s** |
| Loading save → in cab | 9.11 s | 7.32 s | −1.8 s |
| **Total: mods ready → drivable** | **60.97 s** | **29.23 s** | **−31.7 s** |

Almost all of the gain sits in one stage: **1.8 million** "does this file exist" probes per load are answered from memory instead of entering the kernel.

---

## Why it is slow

ETS2's virtual file system has no resource index. Every asset request walks the mod layers in priority order, asking each one "do you have this file?", and stops at the first hit.

A measured startup produced **4.25 million** file operations:

| Result | Count | Share |
|---|---:|---:|
| `PATH NOT FOUND` | 4,196,372 | **98.6 %** |
| `SUCCESS` | 33,328 | 0.8 % |
| `NAME NOT FOUND` | 19,125 | 0.4 % |

Only 33 thousand of them actually found a file. 68 mods means 68 layers, and it takes 29 probes on average to hit one.

The counter-intuitive part: **cost has nothing to do with mod size**. A 9-file tweak mod and a 32 GB graphics pack with 4444 files produce almost the same number of file events (~145 k each), because every mod layer gets asked about the same 76 k base resources.

What this tool does is keep a workshop path table inside the game process and answer "does not exist" in user space, without ever entering the kernel.

---

## Install

### 1. Download

Grab `ets2fast-v1.0.0-win64.zip` from [Releases](../../releases) and extract it anywhere.

### 2. Enable

Double-click **`ets2fast.exe`**.

> The menu is in Chinese. Here is what the keys do:

| Key | Meaning |
|---|---|
| `1` | Enable auto-inject — starts the background daemon and registers it to run at logon |
| `2` | Disable auto-inject — removes the autostart entry, stops the daemon, back to manual mode |
| `3` | Show status and the tail of both log files |
| `0` | Exit |

Press **`1`**, then Enter. That is it — launching the game from Steam now injects automatically, and it keeps working after a reboot.

### 3. Disable (optional)

Open the menu again and press **`2`**. You are back to manual mode: start `inject.exe` first, then launch the game from Steam.

### After moving the folder

The menu detects that the autostart entry still points at the old path and says so. Press `1` once to refresh it.

---

## How it works

Pressing `1` does two things:

1. Starts `ets2fastd.exe`, a windowless background daemon
2. Registers it under `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`

The daemon polls for `eurotrucks2.exe` every 250 ms. Finding it is not enough to inject: it waits until the game has created its first visible top-level window (30 s cap) and only then attaches. The next section explains why.

It also records its own PID and whether it landed inside a job object (`inJob`), and writes a heartbeat every 5 minutes. If it ever gets killed silently, the log shows the exact moment it stopped.

On top of that, **every time `ets2fast.exe` opens it checks whether the daemon is still alive** and starts it again if not — the safety net for a daemon killed by anything outside our control.

**Why a daemon instead of wrapping Steam's launch options with `%command%`:**

| | Wrapper via launch options | Background daemon |
|---|---|---|
| Rewrites Steam config | Yes (and Steam must be closed or it gets overwritten) | **No** |
| Steam in-game overlay | May break — the game becomes a child process | **Unaffected** |
| Launching from a desktop shortcut | Not covered | **Covered** |
| Requires administrator | Depends on install location | **No, no UAC prompt** |

> Compatibility note: if you used the old `--launch` wrapper, `ets2fast.exe` still understands it — it forwards the call to `ets2fastd.exe` and passes the exit code back, so Steam keeps tracking the game correctly. The menu also checks for a leftover wrapper entry when you press `1` and warns you.

---

## Injection timing and high-DPI displays

**If your display scaling is anything other than 100%, this section is for you.**

Earlier builds injected the moment `eurotrucks2.exe` showed up in the process list. That made the game lose its DPI awareness: on a 125% / 150% display it treats physical pixels as logical ones, Windows then enlarges the window by the scaling factor, and the picture ends up larger than the screen with all four edges cropped.

Measured on 2560×1600 @ 125%:

| Condition | Window | Window DPI |
|---|---|---|
| No injection | 2560×1600 @ (0,0) | 120 |
| Old build, injected | **3200×2000 @ (−320,−200)** | **96** |

`3200 = 2560 × 1.25`, exactly the scaling factor.

**The fix**: wait for the main window before attaching. The DPI context is decided when the first window is created, so injecting afterwards no longer touches it.

There is also a fallback inside the DLL that explicitly asks for a per-monitor v2 context. It takes effect when the process has not locked one yet:

```
dpi: context set to per-monitor v2
dpi: context already locked (err=5), left unchanged
```

The second line is the normal case — the game has already declared its own.

**Why the delay costs nothing**: the expensive part of loading is mod enumeration and resource probing, which happens after the main window appears and runs for tens of seconds. The log proves it:

```
hook installed
stats: blocked=0            <- 60 s in, not a single probe yet
stats: blocked=1,547,456    <- the burst happens by 120 s
```

Still zero at 60 s means the game had not reached mod enumeration. Waiting 3–9 s for the window is comfortably early.

---

## Portability

* **No drive letters.** The DLL takes the path of the process it was loaded into (`eurotrucks2.exe`), walks up until it finds a directory named `steamapps`, then builds `workshop\content\227300`. Any drive, any Steam library, zero configuration.
* **No runtime dependencies.** All four binaries are fully static. They only import `KERNEL32` and the UCRT (`api-ms-win-crt-*`) that ships with Windows 10/11. No VC++ redistributable, no MinGW runtime.
* **No administrator rights.** Autostart is written to `HKEY_CURRENT_USER` only.

Unusual layouts can be forced with an environment variable:

```cmd
set ETS2FAST_ROOT=H:\SteamLibrary\steamapps\workshop\content\227300
```

---

## Project layout

This repository contains sources only. Ready-to-run builds are on the [Releases](../../releases) page.

```
ets2fast/
├── src/
│   ├── main.cpp       DLL injected into the game: path index + NtCreateFile hook
│   ├── launcher.cpp   daemon / %command% wrapper / one-shot injector
│   └── setup.cpp      CMD menu: toggles auto-inject, manages the autostart entry
├── tools/
│   └── logtime.py     load-time breakdown (run after quitting the game)
├── README.md          简体中文 (Chinese, main document)
├── README.en.md       English (this file)
└── LICENSE
```

---

## Logs and diagnostics

| File | Contents |
|---|---|
| `ets2fast.log` | Written by the DLL: which workshop root was picked, index size, whether the hook installed, a `blocked/passed` counter every 60 s |
| `ets2fast-launcher.log` | Written by the daemon / injector: process found, injection result, exit code |

A healthy log looks like this:

```
=== ets2fast attach ===
root (derived from game path): G:\steam\steamapps\workshop\content\227300\
index entries: 16225
thunk form detected, real impl = 00007ffcdbbece10
stub patched, len=8, suspended threads: 32
hook installed
stats: blocked=1804170 passed=4572 index=16225
```

`blocked` climbing fast while `passed` stays flat means the interception is working.

If you see `root: NOT FOUND - no hook installed`, or `blocked` stays at 0, the hook did not take — please open an issue with both logs attached.

Debug mode (dumps the first 300 blocked paths):

```cmd
set ETS2FAST_DEBUG=1
```

### Daemon log

A healthy session in `ets2fast-launcher.log`:

```
daemon: started pid=16152 inJob=0
daemon: injected into pid 21280
daemon: hook is live
daemon: game closed, ready for the next launch
daemon: alive, uptime 300 s
```

**There is an `alive, uptime N s` line every 5 minutes.** If the daemon ever
goes missing, the timestamp of the last line tells you exactly when it stopped —
no guessing.

The `inJob` field is worth watching:

| Value | Meaning |
|---|---|
| `inJob=0` | Independent process, as intended |
| `inJob=1` | It inherited a job object. If that job closes when its owner exits, Windows **kills the daemon silently** — not a single line in the log |

When `inJob=1` shows up, the program first tries to relaunch itself through the
shell to escape (the log says `relaunched through the shell`). If it is still
`inJob=1` afterwards, your system has a global job restriction and the recovery
below is what keeps things working.

### If the daemon disappears

Just open `ets2fast.exe`. On startup it checks whether auto inject is configured
while the daemon is missing, and starts it again, with a notice at the top:

```
  * 后台守护之前已停止，已自动重新启动（PID 10744）
  (daemon had stopped, restarted automatically)
```

The status line also shows the daemon's PID and uptime. "刚刚启动" (just
started) means it was restarted; a long uptime means it has been fine all along.

---

## Safety

* Only paths that are **provably absent** are refused: every level starting from the mod id must exist in the index. An unrecognised level always falls through. An incomplete index cannot cause false negatives.
* Error codes match Windows exactly: a missing leaf returns `2`, a missing parent returns `3`. Any game logic that branches on the error code sees the same values as without the hook.
* Any request with write access is always passed through.
* If the index is empty or scanning fails, **the hook is not installed at all** — behaviour is identical to vanilla.
* Directory watching: subscribing or unsubscribing mods while the game runs (Steam downloads, then "Confirm changes" triggers a hot reload) is detected — interception stops immediately, waits for a few seconds of quiet, rebuilds the index and resumes. No game restart needed.
* A failed injection never breaks the game. Worst case: no speedup.
* The daemon writes only its own logs and one `HKEY_CURRENT_USER` autostart value. It does not touch system directories, the game folder, or any mod.

---

## Compatibility

Tested with these third-party components present in `bin\win_x64`:

| File | What it is | Hooks file APIs? |
|---|---|---|
| `dxgi.dll` | ReShade 6.8.0 | No (graphics / input only) |
| `steam_api64.dll` | cream_api 5.1.0 | No (Steam API proxy) |
| `version.dll` | Juij loader | No |
| `winmm.dll` | Juij loader | No |
| `dstorage.dll` | SCS DirectStorage | No |

**Hook chaining** was verified as well: if another tool hooked `NtCreateFile` first, this DLL reads the current jump target as the "original implementation" and calls through it, so both hooks fire and neither is overwritten. In the reverse order the worst case is that this tool stops working — no errors, no crashes.

---

## Known limitations

* **Absolute paths only.** The game currently uses nothing else. If a relative path ever appears, that lookup simply goes unaccelerated — correctness is unaffected.
* **`NtCreateFile` only.** If SCS ever routes mod reads through DirectStorage (currently used for official `.scs` only), interception stops applying — the speedup shrinks, nothing breaks.
* Windows feature updates can change how `ntdll` exports look. Both the Win11 thunk form (`48 B8 <imm64> FF E0`) and the classic syscall stub (`4C 8B D1 B8 ...`) are supported and were tested successfully.
* The daemon stays resident using about 9 MB of RAM and polls the process list every 250 ms.

---

## Tested on

| | |
|---|---|
| OS | Windows 11 Pro, build 26300 |
| Game | Euro Truck Simulator 2 1.62.3.2s (Steam) |
| Mods | 58 workshop subscriptions |
| Hardware | Ryzen 7 3700X / RTX 3070 / 16 GB |
| Storage | Colorful CN600 1 TB PRO (PCIe 3.0, DRAM-less) |

---

## License

[MIT](LICENSE)
