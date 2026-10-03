# appmenu_test — Krita 6 / Qt 6 全局菜单空白的最小可视复现

> 英文译本：[`README.en.md`](README.en.md)（仅供阅读；**本文中文版为准**）。

一个窗口化的最小复现程序：在**真正的 `QApplication` 之前**创建一个一次性
`QGuiApplication`（这正是 Krita 的 OpenGL 探测器
`KisOpenGLModeProber::probeFormat()` 所做的事），然后打开一个普通窗口。

- **Qt 6**：Plasma 面板的 Application Menu（全局菜单）**空白**，窗口内也**没有**菜单栏。
- **Qt 5**：全局菜单正常显示 File / Edit / Help。

程序本身**不打印任何东西**（除 `--help` 与 `--verbose`），直接看窗口和面板即可。可分别用 Qt5 / Qt6 构建。

## 现象

| | 面板全局菜单 | 窗口内菜单栏 |
|---|---|---|
| `plain`（对照） | File / Edit / Help | 无（正常：菜单交给了全局菜单） |
| `probe` / `krita`（Qt 6） | **空白** | **无** ← BUG |
| `probe` / `krita`（Qt 5） | File / Edit / Help | 无 |

`KDE_NO_GLOBAL_MENU=1` 会让菜单栏退回窗口内（所有模式都一样），这是 Krita 已知的临时绕过办法。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure      # 冒烟测试：两个变体的 --help
```

在同一棵构建树里按可用性分别产出 Qt6 / Qt5 两份：

- `build/appmenu_test-qt6`
- `build/appmenu_test-qt5`

只想要其中一份：

```sh
cmake -S . -B build -DAPPMENU_TEST_QT5=OFF      # 只编 Qt6
cmake -S . -B build -DAPPMENU_TEST_QT6=OFF      # 只编 Qt5
```

## 运行

```sh
./build/appmenu_test-qt6 plain      # 对照：面板全局菜单应显示 File/Edit/Help
./build/appmenu_test-qt6 probe      # 复现 BUG
./build/appmenu_test-qt6 krita      # 最贴近 Krita：临时 QCoreApplication + 3 个探测应用
./build/appmenu_test-qt6 core       # 只加临时 QCoreApplication（无害）
./build/appmenu_test-qt5 probe      # Qt5 同路径：正常
```

> 如果 shell 里存在 `SESSION_MANAGER` 而 `~/.config` 不可写，Qt 会弹出
> 「Configuration file ... not writable」模态框卡住程序。用
> `env -u SESSION_MANAGER XDG_CONFIG_HOME=/tmp/appmenu-cfg ./build/appmenu_test-qt6 probe` 规避。

### 选项

| 选项 | 作用 |
|---|---|
| `--probes N` | 探测用 `QGuiApplication` 的个数（`probe` 默认 1，`krita` 默认 3） |
| `--bare-probe` | 探测应用只 `new QGuiApplication`，不创建 QWindow（**这样不会复现**） |
| `--block-a11y` | 探测应用里把 `AT_SPI_BUS_ADDRESS` 设为非空，a11y 桥因此不再碰 session bus —— **有效的 workaround** |
| `--reset-a11y` | 探测结束后 `disconnectFromBus("a11y")`，清掉探测应用留下的死 a11y 连接 |
| `--no-portal-guard` | 不在探测应用里设 `QT_NO_XDG_DESKTOP_PORTAL=1`（**Qt5 也会坏**；与 `--block-a11y` 一起用时也必须保留） |
| `--no-workaround` | 不调用 `QGuiApplication::setDesktopSettingsAware(false)`（Qt6 下无影响） |
| `--block-dbus` | 探测期把 `DBUS_SESSION_BUS_ADDRESS` 指向死地址（**实测无效且有害**，见下节） |
| `--reset-bus` | 探测后 `disconnectFromBus("qt_default_session_bus")`（**实测无效**，见下节） |
| `--verbose` | 往 stderr 打印阶段标记、`winId`、`isNativeMenuBar` 与 `/MenuBar/1` 注册状态（脚本化诊断用） |
| `--wayland` | 强制 `QT_QPA_PLATFORM=wayland`（默认沿用 Krita 的 xcb / XWayland） |
| `--hidden` | 不显示窗口（供脚本化诊断用） |
| `--help` | 用法 |

想看修好的样子（面板全局菜单会出现 File/Edit/Help）：

```sh
./build/appmenu_test-qt6 probe --block-a11y --reset-a11y
```

## 复现的必要条件（实测）

| 探测应用里做了什么 | Qt 6.11.2 | Qt 5.15.19 |
|---|---|---|
| 只 `new QGuiApplication`（`--bare-probe`） | 0 / 0 正常 | 0 / 0 正常 |
| **+ 创建并在稍后销毁一个 `QWindow`**（默认，等同 Krita prober） | **124 / 124 复现** | 0 / 0 正常 |
| + 同上，但不设 portal guard | 124 / 124 | **124 / 124（Qt5 也坏）** |

（数值 = 从另一个进程对导出对象做 `Introspect` / `AboutToShow` 的返回码；`124` 表示超时无应答。）

## 能不能"在探测阶段加个开关"就修掉它？（实测）

可以，而且**不需要屏蔽整条 session bus**：只在探测应用存活期间把无障碍（at-spi）桥从 session bus
上引开即可。原理在 Qt6 的 `src/gui/accessible/linux/dbusconnection.cpp`：

```cpp
QAtSpiDBusConnection::QAtSpiDBusConnection(QObject *parent) : ... {
    QByteArray addressEnv = qgetenv("AT_SPI_BUS_ADDRESS");
    if (!addressEnv.isEmpty()) {            // 非空就完全绕过 session bus
        m_enabled = true;
        connectA11yBus(QString::fromLocal8Bit(addressEnv));
        return;
    }
    QDBusConnection c = QDBusConnection::sessionBus();   // ← 探测期建立连接的元凶
    ...
```

于是探测应用不再建立 session bus 连接，进程里第一条连接由**真实应用**建立 →
`enableDispatchDelayed(qApp)` 落在真实 `qApp` 上 → `exec()` 时投递恢复 → 全局菜单正常。

| 配置（`probe` 模式，Qt 6.11.2 实测） | `isNativeMenuBar` | `/MenuBar/1` 注册 | 跨进程 `Introspect`/`AboutToShow` | 结论 |
|---|---|---|---|---|
| 基线 | 1 | 1 | **124 / 124** | 复现 bug |
| `--block-a11y` | 1 | 1 | **0 / 0** | ✅ 修好 |
| `--block-a11y --reset-a11y` | 1 | 1 | **0 / 0** | ✅ 修好（且不留死 a11y 连接） |
| `--block-a11y --no-portal-guard` | 1 | 1 | 124 / 124 | portal guard 仍然必需 |
| `--no-portal-guard` | 1 | 1 | 124 / 124 | portal 会建立连接 |
| `--bare-probe` | 1 | 1 | 0 / 0 | 探测里不建窗口 → 本来就不复现 |
| `--block-dbus` | **0** | **0** | 无对象可探 | ❌ 整条 session bus 变死（连全局菜单都没了） |
| `--reset-bus` | 1 | 1 | 124 / 124 | ❌ 无效 |

两个失败方案的原因都在 Qt 源码里：

* `--block-dbus`：`QDBusConnectionManager::doConnectToStandardBus()` 里 `setConnection(name, d)`
  是在 `d->setConnection(c /* == nullptr */, error)` **之前**无条件执行的，所以"连不上"也会被缓存；
  `busConnection()` 之后永远返回这条死连接 ⇒ 整个进程失去 session bus（不只是全局菜单，
  KConfig/KIO/通知等一起失效）。
* `--reset-bus`：`QDBusConnection::disconnectFromBus("qt_default_session_bus")` →
  `QDBusConnectionManager::removeConnection()` 只从 `connectionHash` 删除，**不动 `defaultBuses[]`**
  （全项目只有 manager 构造函数把它置 nullptr），而 `busConnection()` 只要 `defaultBuses[type]` 非空就直接返回 ⇒
  真实应用拿到的还是那条 `delivery suspended` 的连接。对比之下 `--reset-a11y` 有效，是因为 `a11y`
  只是 `connectionHash` 里的命名连接，不在 `defaultBuses[]` 里，可以真正被删掉。

要点：

* `AT_SPI_BUS_ADDRESS` 必须**只在探测应用存活期间**设置。全程设置虽然也能"修好"，但会把整个进程的
  无障碍引到假地址（实测 `AT_SPI_BUS_ADDRESS=unix:path=...` 进程级设置同样是 0/0/0）。本程序用 RAII
  `EnvironmentSetter` 自动恢复。
* 没有 `Qt::AA_*` 属性或公开 API 能"关掉无障碍"：实测进程级 `QT_ACCESSIBILITY=0`、`NO_AT_BRIDGE=1`
  都无效（桥照样建立 session bus）；`QT_LINUX_ACCESSIBILITY_ALWAYS_ON` 是"强制开"。
* 这仍是 workaround：依赖 at-spi 桥"`AT_SPI_BUS_ADDRESS` 非空即 early-return"这一实现细节。
  根因修法还是不要在真实应用之前创建 `QGuiApplication`（子进程探测 / 应用内探测）。

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
   （XDG portal）挡住了；而 Qt6 下探测应用**销毁那个 `QWindow`** 时仍会打开 session bus：
   `QWindow::~QWindow()` 调用 `QAccessible::isActive()`，后者经 QPA 插件懒加载 at-spi 桥
   （`src/gui/accessible/linux/dbusconnection.cpp`）→ `QDBusConnection::sessionBus()`。
   探测窗口只 `create()`、从不 `show()`，所以这是该应用整个生命里唯一一次无障碍查询；
   现有的两个 guard 都挡不住它，只有 `AT_SPI_BUS_ADDRESS`（见上节）能拦住。

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

完整原因分析（QtDBus 内部机制、实测矩阵、KDE 侧缓解措施与建议修法）：
[`BUGREPORT.md`](BUGREPORT.md)（中文，以此为准）· [`BUGREPORT.en.md`](BUGREPORT.en.md)（English）。

---

> 英文译本：[`README.en.md`](README.en.md)
