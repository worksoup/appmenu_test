# appmenu_test — Krita 6 / Qt 6 全局菜单空白的最小可视复现

一个窗口化的最小复现程序：在**真正的 `QApplication` 之前**创建一个一次性
`QGuiApplication`（这正是 Krita 的 OpenGL 探测器
`KisOpenGLModeProber::probeFormat()` 所做的事），然后打开一个普通窗口。

- **Qt 6**：Plasma 面板的 Application Menu（全局菜单）**空白**，窗口内也**没有**菜单栏。
- **Qt 5**：全局菜单正常显示 File / Edit / Help。

程序本身**不打印任何东西**（除 `--help`），直接看窗口和面板即可。可分别用 Qt5 / Qt6 构建。

## 现象

| | 面板全局菜单 | 窗口内菜单栏 |
|---|---|---|
| `plain`（对照） | File / Edit / Help | 无（正常：菜单交给了全局菜单） |
| `probe` / `krita`（Qt 6） | **空白** | **无** ← BUG |
| `probe` / `krita`（Qt 5） | File / Edit / Help | 无 |

`KDE_NO_GLOBAL_MENU=1` 会让菜单栏退回窗口内（所有模式都一样），这是 Krita 已知的临时绕过办法。

## 构建

```sh
./build.sh          # 需要 qt6-base / qt5-base 的开发文件（pkg-config）
```

产出：

- `appmenu_test-qt6`
- `appmenu_test-qt5`

## 运行

```sh
./appmenu_test-qt6 plain      # 对照：面板全局菜单应显示 File/Edit/Help
./appmenu_test-qt6 probe      # 复现 BUG
./appmenu_test-qt6 krita      # 最贴近 Krita：临时 QCoreApplication + 3 个探测应用
./appmenu_test-qt6 core       # 只加临时 QCoreApplication（无害）
./appmenu_test-qt5 probe      # Qt5 同路径：正常
```

> 如果 shell 里存在 `SESSION_MANAGER` 而 `~/.config` 不可写，Qt 会弹出
> 「Configuration file ... not writable」模态框卡住程序。用
> `env -u SESSION_MANAGER XDG_CONFIG_HOME=/tmp/appmenu-cfg ./appmenu_test-qt6 probe` 规避。

### 选项

| 选项 | 作用 |
|---|---|
| `--probes N` | 探测用 `QGuiApplication` 的个数（`probe` 默认 1，`krita` 默认 3） |
| `--bare-probe` | 探测应用只 `new QGuiApplication`，不创建 QWindow（**这样不会复现**） |
| `--no-portal-guard` | 不在探测应用里设 `QT_NO_XDG_DESKTOP_PORTAL=1`（**Qt5 也会坏**） |
| `--no-workaround` | 不调用 `QGuiApplication::setDesktopSettingsAware(false)`（Qt6 下无影响） |
| `--wayland` | 强制 `QT_QPA_PLATFORM=wayland`（默认沿用 Krita 的 xcb / XWayland） |
| `--hidden` | 不显示窗口（供脚本化诊断用） |
| `--help` | 用法 |

## 复现的必要条件（实测）

| 探测应用里做了什么 | Qt 6.11.2 | Qt 5.15.19 |
|---|---|---|
| 只 `new QGuiApplication`（`--bare-probe`） | 0 / 0 正常 | 0 / 0 正常 |
| **+ 创建 QWindow**（默认，等同 Krita prober） | **124 / 124 复现** | 0 / 0 正常 |
| + 创建 QWindow，但不设 portal guard | 124 / 124 | **124 / 124（Qt5 也坏）** |

（数值 = 从另一个进程对导出对象做 `Introspect` / `AboutToShow` 的返回码；`124` 表示超时无应答。）

## 根因摘要

1. Qt6 的 `QDBusConnectionManager::busConnection()` 在 `qApp` 已存在时，会以
   **“先挂起投递”**（`suspendedDelivery`）模式创建 session bus 连接，并调用
   `result->enableDispatchDelayed(qApp)`；后者是**投递给 `qApp` 的排队调用**，
   要在该应用的 event loop 里才会执行 `setDispatchEnabled(true)`。
2. 探测应用在进入 event loop 之前就被销毁 → 这个排队调用随接收者一起消失 →
   连接永久停在 `dispatchEnabled == false`。连接对象存活在全局
   `QDBusConnectionManager` 里并被缓存（`defaultBuses[]`），真实应用再取到它时
   `if (defaultBuses[type]) return defaultBuses[type];` 直接返回，不会再安排“恢复”。
3. 于是 `handleMessage()` 里 `if (!dispatchEnabled && !isLocal)` 把所有外部方法调用
   丢进 `pendingMessages` 且**不回包** → Plasma 的 applet 只能超时 → 全局菜单空白。
   只有连接层内建的 `Peer.Ping` 仍有应答（所以只测 Ping 会被骗过）。
4. Qt5 里同样的“恢复投递”用的是 `QTimer::singleShot(0, enabler, ...)`，但**只要连接是在
   探测应用存活期间建立的，Qt5 同样会永久挂起**（上表最后一行）。Qt5 之所以在 Krita 上
   没事，是因为 Krita 的 prober 设了 `QT_NO_XDG_DESKTOP_PORTAL=1`，把当时唯一的触发者
   （XDG portal）挡住了；而 Qt6 下探测应用**创建 QWindow** 时仍会经无障碍（at-spi）等
   平台路径打开 session bus，挡不住。

对应 Krita 代码：`libs/ui/opengl/KisOpenGLModeProber.cpp` 的 `probeFormat()`
（`new QGuiApplication` + `QWindow surface; surface.create();`），调用链来自
`krita/main.cc` → `KisOpenGL::selectSurfaceConfig()`。

## 相关记录

- KDE Bug 483170 「appmenu (global menu) doesn't work with krita on plasma 6」
- KDE Bug 515889 「[qt6] Application Menu is unavailable on Krita 6」
- Krita 历史修复 `8d6a2a5d12`（"Do not load the platform theme when created a test
  QApplication", BUG 408015），其 `setDesktopSettingsAware(false)` 在 Qt6 已不足以挡住
  session bus 连接的建立
- Qt 源码：`src/dbus/qdbusconnectionmanager.cpp`（`busConnection` / `connectToBus`）、
  `src/dbus/qdbusintegrator.cpp`（`enableDispatchDelayed` / `handleMessage`）
