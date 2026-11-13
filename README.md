# ets2fast

**Euro Truck Simulator 2 载入加速** — 把「等读档」从 1 分钟压到 30 秒以内，不修改游戏文件，不修改任何 mod。

[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078D6)](#)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

**简体中文** | [English](README.en.md)

---

## 效果

同一台机器、同样 58 个 mod、同一次载入，装与不装的实测对比：

| 阶段 | 无工具 | 有工具 | 差值 |
|---|---:|---:|---:|
| 主菜单出现 | 8.42 s | 8.93 s | — |
| mod 枚举（58 个） | 25.05 s | 23.27 s | −1.8 s |
| 挂载 58 个包 | 0.33 s | 0.35 s | ≈0 |
| 交通 / 城市 / 预制件 | 6.86 s | 6.37 s | −0.5 s |
| **公司货物分配** | **38.13 s** | **15.20 s** | **−22.9 s** |
| 读存档 → 进驾驶室 | 9.11 s | 7.32 s | −1.8 s |
| **合计：mod 就绪 → 可驾驶** | **60.97 s** | **29.23 s** | **−31.7 s** |

收益集中在「公司货物分配」这一段：一次载入里有 **180 万次**「查一个不存在的文件」被内存查表直接挡掉，不再进内核。

---

## 为什么慢

ETS2 的虚拟文件系统没有资源索引。它的做法是：来一个资源请求，就按 mod 优先级**逐层问「你有这个文件吗」**，找到即停。

实测一次启动产生 **425 万次**文件操作，其中：

| 结果 | 次数 | 占比 |
|---|---:|---:|
| `PATH NOT FOUND` | 4,196,372 | **98.6 %** |
| `SUCCESS` | 33,328 | 0.8 % |
| `NAME NOT FOUND` | 19,125 | 0.4 % |

真正找到文件的只有 3.3 万次，其余全是无用探测。68 个 mod 就是 68 层，平均试到 29 层才命中。

更反直觉的是：**开销与 mod 的体积无关**。一个只有 9 个文件的数值 mod，和一个 32 GB、4444 个文件的画质 mod，产生的文件事件几乎相同（都在 14.5 万次上下）——因为游戏对每个 mod 层都要把那 7.6 万个基础资源试一遍。

本工具的做法：在游戏进程内维护一份 workshop 路径表，路径不存在时直接在用户态返回，不交给内核。

---

## 安装使用

### 1. 下载

从 [Releases](../../releases) 下载 `ets2fast-v1.0.0-win64.zip`，解压到任意位置（建议路径不带空格和中文，但要带也无妨）。

### 2. 开启

双击 **`ets2fast.exe`**，看到这个界面：

```
  ==========================================================
     ETS2 载入加速  -  设置           ets2fast
  ==========================================================
  自动注入: 未开启（手动方式）

  [1] 开启自动注入（后台守护 + 开机自启）
  [2] 关闭自动注入（改回先开 inject.exe）
  [3] 查看状态与日志
  [0] 退出
```

按 **`1`** 再回车。完成。以后从 Steam 点「开始游戏」就会自动注入，开机后也自动生效。

### 3. 关闭（可选）

再双击 `ets2fast.exe`，按 **`2`** 回车，就回到手动方式：每次玩之前先双击 `inject.exe`，再从 Steam 启动游戏。

### 目录被移动后

菜单会提示「开机自启里记录的是旧路径」，重新按一次 `1` 即可。

---

## 工作方式

`ets2fast.exe` 按 1 之后会做两件事：

1. 在后台启动 `ets2fastd.exe`（无窗口守护进程）
2. 把它登记到 `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`

守护进程每 250 ms 检查一次 `eurotrucks2.exe` 是否存在，发现即注入 `ets2fast.dll`，然后继续等下一次启动。

**为什么不用 `%command%` 包装 Steam 启动选项：**

| | 包装启动选项 | 后台守护 |
|---|---|---|
| 需要改 Steam 配置 | 是（且要关掉 Steam 才不会被覆盖） | **否** |
| Steam 游戏内叠加界面 | 可能失效（游戏成了子进程） | **正常** |
| 从桌面快捷方式启动 | 不生效 | **生效** |
| 需要管理员权限 | 视安装位置 | **否，无 UAC** |

> 兼容性：如果你之前用过旧版本的 `--launch` 包装方式，`ets2fast.exe` 收到 `--launch` 时会自动转交给 `ets2fastd.exe` 处理并透传退出码，不会出错。菜单按 `1` 时会检查 Steam 启动选项是否有残留并提醒。

---

## 可移植性

* **不依赖盘符**：DLL 从自己被加载进的那个进程（`eurotrucks2.exe`）的位置逐级上溯，找到名为 `steamapps` 的目录，再拼出 `workshop\content\227300`。游戏装在哪个盘、哪个 Steam 库都不需要配置。
* **不依赖运行库**：四个产物全部静态链接，只依赖 `KERNEL32` 和 Win10/11 自带的 UCRT（`api-ms-win-crt-*`）。没有 VC++ 运行库或 MinGW 运行库依赖。
* **不需要管理员**：开机自启只写 `HKEY_CURRENT_USER`。

特殊布局可以用环境变量强制指定：

```cmd
set ETS2FAST_ROOT=H:\SteamLibrary\steamapps\workshop\content\227300
```

---

## 项目结构

本仓库只包含源码。可直接运行的成品请到 [Releases](../../releases) 下载。

```
ets2fast/
├── src/
│   ├── main.cpp       注入进游戏的 DLL：路径索引 + NtCreateFile hook
│   ├── launcher.cpp   守护进程 / 包装器 / 手动注入器（一份源码编出两个 exe）
│   └── setup.cpp      CMD 菜单：开关自动注入、管理开机自启
├── tools/
│   └── logtime.py     读档耗时分析（跑完游戏后运行）
├── README.md          简体中文（本文件）
├── README.en.md       English
└── LICENSE
```

---

## 日志与诊断

| 文件 | 内容 |
|---|---|
| `ets2fast.log` | DLL 写的：选了哪个 workshop 目录、索引条数、hook 是否装上、每 60 秒一行 `blocked/passed` 计数 |
| `ets2fast-launcher.log` | 守护进程 / 注入器写的：发现游戏、注入结果、退出码 |

正常的日志长这样：

```
=== ets2fast attach ===
root (derived from game path): G:\steam\steamapps\workshop\content\227300\
index entries: 16225
thunk form detected, real impl = 00007ffcdbbece10
stub patched, len=8, suspended threads: 32
hook installed
stats: blocked=1804170 passed=4572 index=16225
```

`blocked` 迅速增长、`passed` 基本不动，说明拦截在生效。

看到 `root: NOT FOUND - no hook installed` 或 `blocked=0`，说明挂钩没成功，请附上日志提 issue。

调试模式（记录前 300 条被拦路径）：

```cmd
set ETS2FAST_DEBUG=1
```

---

## 安全性设计

* 只拦**能证明不存在**的路径：从 mod id 开始逐级必须存在，遇到不认识的层级一律放行。索引不完整也不会误伤。
* 返回的错误码与系统原生一致：末级文件缺失返回 `2`，中间目录缺失返回 `3`。游戏内部按错误码分支的逻辑不会改变走向。
* 任何带写权限的打开请求一律放行。
* 索引为空或扫描失败时**根本不安装 hook**，游戏行为等同原版。
* 目录监听：游戏运行中订阅/取消 mod（Steam 下载后点「确认更改」热重载）会被检测到——立即停止拦截、等几秒静默、重建索引、自动恢复，不需要重启游戏。
* 注入失败不会影响游戏：最坏情况就是没有加速，游戏照常运行。
* 守护进程只写自己的日志和 `HKEY_CURRENT_USER` 里那一个开机自启项，不碰系统目录、不碰游戏目录、不碰任何 mod。

---

## 兼容性

游戏 `bin\win_x64` 目录里同时存在这些第三方组件时已实测无冲突：

| 文件 | 身份 | 是否 hook 文件 API |
|---|---|---|
| `dxgi.dll` | ReShade 6.8.0 | 否（hook 图形/输入） |
| `steam_api64.dll` | cream_api 5.1.0 | 否（只代理 Steam API） |
| `version.dll` | Juij 加载器 | 否 |
| `winmm.dll` | Juij 加载器 | 否 |
| `dstorage.dll` | SCS 官方 DirectStorage | 否 |

**链式 hook** 也验证过：若别的工具先 hook 了 `NtCreateFile`，本工具会读取当时的跳转目标作为「原始实现」，接在它之上调用，两个钩子都会触发，不会互相覆盖。反向（别人在本工具之后再 hook）最坏只是本工具失效，不会报错。

---

## 已知限制

* 只处理**绝对路径**。游戏目前全是绝对路径；若将来某处用相对路径，那部分只是不加速，不影响正确性。
* 只 hook `NtCreateFile`。若游戏改用 DirectStorage 读 mod（目前仅用于官方 `.scs`），拦截会失效——表现为加速变小，不会出错。
* Windows 大版本更新可能改变 `ntdll` 导出形式。代码同时支持 Win11 的 thunk 形式（`48 B8 <imm64> FF E0`）和传统 syscall stub 形式（`4C 8B D1 B8 ...`），两种都实测装成功。
* 守护进程常驻，内存占用约 9 MB，每 250 ms 查一次进程列表。

---

## 实测环境

| 项 | 配置 |
|---|---|
| 系统 | Windows 11 专业版 Build 26300 |
| 游戏 | Euro Truck Simulator 2 1.62.3.2s（Steam） |
| mod | 58 个创意工坊订阅 |
| 硬件 | Ryzen 7 3700X / RTX 3070 / 16 GB |
| 磁盘 | Colorful CN600 1TB PRO（PCIe 3.0 无缓存） |

---

## 许可

[MIT](LICENSE)
