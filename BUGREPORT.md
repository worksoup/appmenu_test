# BUG REPORT — Krita 6 / Qt 6: Plasma Application Menu stays empty (no global menu, no in-window menu bar)

Reported: **2026-10-03 15:02 CST (07:02 UTC)**
Root-cause analysis and minimal reproducer by the **DeepSeek Harness coding agent**, in session with the reporter.
Environment: Arch Linux, KDE Plasma (Wayland session; Krita runs on XWayland by default), **Qt 6.11.2**, KF6 6.30.0,
`plasma-integration` 6.7.5; Krita **master `351297cdce` (6.1.0-prealpha)** and distro **6.0.4**; Qt 5.15.19 for comparison.

---

## English

### Summary

On Krita 6 (Qt 6) the Plasma *Application Menu* widget stays **empty**, and Krita's own menu bar is **not drawn inside the
window** either. `KDE_NO_GLOBAL_MENU=1` makes the in-window menu bar appear. Krita 5.x (Qt 5) is unaffected.

The menu is in fact exported correctly — the bug is that the process **never answers the D-Bus calls** that read it.

### What Krita does

`KisOpenGLModeProber::probeFormat()` (`libs/ui/opengl/KisOpenGLModeProber.cpp`, reached from `krita/main.cc` via
`KisOpenGL::selectSurfaceConfig()`) creates a **throw-away `QGuiApplication`** — plus a `QWindow` and a `QOpenGLContext` —
*before* the real `KisApplication` exists, in order to probe the OpenGL renderers. Krita already tries to keep that
throw-away application away from D-Bus:

* `QGuiApplication::setDesktopSettingsAware(false)` (so the KDE platform theme, and its `KConfigWatcher`, are not created), and
* `QT_NO_XDG_DESKTOP_PORTAL=1` (so the XDG-desktop-portal settings watcher is not created).

On Qt 5 that is enough. On Qt 6 it is not: when the throw-away application creates its `QWindow`, Qt 6's
**at-spi accessibility bridge** opens the session bus
(`src/gui/accessible/linux/dbusconnection.cpp`, `QAtSpiDBusConnection::QAtSpiDBusConnection()`):

```cpp
    QByteArray addressEnv = qgetenv("AT_SPI_BUS_ADDRESS");
    if (!addressEnv.isEmpty()) {                 // non-empty -> never touches the session bus
        m_enabled = true;
        connectA11yBus(QString::fromLocal8Bit(addressEnv));
        return;
    }
    QDBusConnection c = QDBusConnection::sessionBus();   // <-- creates the process-wide session bus here
```

So in Qt 6 the session-bus connection is created **while the throw-away application is alive** (`qApp` == the probe app).

### What Qt 6 does (the actual defect)

1. `QDBusConnectionManager::busConnection()` (`src/dbus/qdbusconnectionmanager.cpp`) creates the standard bus connection in
   **suspended delivery** mode whenever a `QCoreApplication` already exists, and caches the raw pointer:

   ```cpp
   bool suspendedDelivery = QThread::isMainThread() && qApp;
   ...
   if (defaultBuses[type])                       // cached pointer, never re-armed
       return defaultBuses[type];
   return defaultBuses[type] = connectToBus(type, name, suspendedDelivery);
   ```

2. `connectToBus()` then schedules the "resume delivery" step as a **queued invocation posted to `qApp`**:

   ```cpp
   if (suspendedDelivery && result && result->connection)
       result->enableDispatchDelayed(qApp);      // qApp == the throw-away application
   ```

   ```cpp
   void QDBusConnectionPrivate::enableDispatchDelayed(QObject *context)   // src/dbus/qdbusintegrator.cpp
   {
       ref.ref();
       QMetaObject::invokeMethod(context, [this]() {          // receiver is qApp
           QMetaObject::invokeMethod(this, &QDBusConnectionPrivate::setDispatchEnabled,
                                     Qt::QueuedConnection, true);
           ...
       }, Qt::QueuedConnection);
   }
   ```

3. Krita destroys the throw-away application **before any event loop runs**, so that queued invocation dies with its
   receiver and `setDispatchEnabled(true)` is never executed. Because the connection object lives in the global
   `QDBusConnectionManager` and is pinned in `defaultBuses[]`, the real application receives the same connection
   (`if (defaultBuses[type]) return defaultBuses[type];`) and nothing ever re-arms it: the connection stays
   `dispatchEnabled == false` forever.

4. Consequently `QDBusConnectionPrivate::handleMessage()` takes this branch for every incoming call from another process:

   ```cpp
   if (!dispatchEnabled && !isLocal) {
       qDBusDebug() << this << "delivery is suspended";
       pendingMessages << amsg;
       return amsg.type() == QDBusMessage::MethodCallMessage;   // "handled", but no reply is ever sent
   }
   ```

   The caller (Plasma's applet) simply times out. `QDBUS_DEBUG=1` prints exactly `delivery is suspended` for each incoming
   `Introspect`/`GetLayout`/`AboutToShow`.

### Why the user-visible symptoms follow

* Qt's `QDBusMenuBar` *does* export the menu: it registers `/MenuBar/N` on the session bus and (on xcb) calls
  `com.canonical.AppMenu.Registrar.RegisterWindow(xid, path)`; Plasma's kded appmenu module even writes
  `_KDE_NET_WM_APPMENU_SERVICE_NAME` / `_OBJECT_PATH` onto Krita's window. All of that was verified to be present and correct.
* But the applet's `GetLayout` / `AboutToShow` on that object never get an answer → the panel shows nothing.
* Because `QMenuBar` was told a native (global) menu bar exists, Qt **hides** the in-window menu bar → there is no menu at all.
* `KDE_NO_GLOBAL_MENU=1` makes `createPlatformMenuBar()` return `nullptr`, so Qt draws the menu bar inside the window
  (that is the commonly used workaround).
* Only the connection-level `org.freedesktop.DBus.Peer.Ping` still answers (it is handled without going through the
  suspended dispatch path), which makes this very easy to misdiagnose — a plain `Ping` "proves" the app is responsive.

### Why Qt 5 is not affected

Qt 5 has the same "delayed enable" idea, but with Krita's workarounds the probe application never opens the session bus:
on Qt 5 the only thing that would have opened it during the probe was the XDG portal, and that is blocked by
`QT_NO_XDG_DESKTOP_PORTAL=1`. (Note: if the connection *is* created while a throw-away application is alive, Qt 5 breaks in
exactly the same way — measured.) On Qt 6 the at-spi bridge opens it as well, and no existing workaround blocks that.

### Minimal reproducer

```cpp
// before the real QApplication, exactly like KisOpenGLModeProber::probeFormat():
{
    int argc = 1;
    QByteArray n("probe"); char *argv[] = { n.data(), nullptr };
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");          // Krita already does this
    QGuiApplication probe(argc, argv);
    QWindow surface;
    surface.setSurfaceType(QSurface::OpenGLSurface);
    surface.create();                                   // <-- this is what makes Qt 6 open the session bus
}
QApplication app(argc, argv);
QMainWindow w; w.menuBar()->addMenu("File")->addAction("Open"); w.show();
```

From a second process, ask the exported object:

```sh
gdbus call --session --dest <krita-or-probe-bus-name> --object-path /MenuBar/N \
      --method com.canonical.dbusmenu.GetLayout 0 1 "@as []"     # -> no reply / timeout
gdbus call --session --dest <krita-or-probe-bus-name> --object-path /MenuBar/N \
      --method com.canonical.dbusmenu.AboutToShow 0             # -> no reply / timeout
gdbus call --session --dest <krita-or-probe-bus-name> --object-path / \
      --method org.freedesktop.DBus.Peer.Ping                   # -> answers
```

A complete windowed reproducer (Qt 5 + Qt 6, CMake) is in this repository (`appmenu-test`):
`MODE = plain | core | probe | krita`; `probe`/`krita` create the throw-away `QGuiApplication`/`QWindow`.

Measured with it (Qt 6.11.2; values are the exit status of a cross-process `Introspect` / `AboutToShow`; 124 = timeout):

| configuration | `isNativeMenuBar` | `/MenuBar/1` registered | cross-process probe |
|---|---|---|---|
| `plain` (control) | 1 | 1 | 0 / 0 |
| `probe` (throw-away `QGuiApplication` only, no window) | 1 | 1 | 0 / 0 |
| **`probe` (throw-away app creates a `QWindow`, i.e. Krita-like)** | 1 | 1 | **124 / 124 (bug)** |
| `probe` + poison `DBUS_SESSION_BUS_ADDRESS` during the probe | **0** | **0** | no object at all (session bus is dead for the whole process) |
| `probe` + `disconnectFromBus("qt_default_session_bus")` after the probe | 1 | 1 | 124 / 124 (no effect) |
| **`probe` + `AT_SPI_BUS_ADDRESS=1` during the probe** | 1 | 1 | **0 / 0 (fixed)** |
| Qt 5, all of the above | 1 | 1 | 0 / 0 |

### Workaround

**Set `AT_SPI_BUS_ADDRESS` to any non-empty value (e.g. `AT_SPI_BUS_ADDRESS=1`) while the throw-away probe application is
alive.** Qt's at-spi bridge then takes the early-return path shown above and never calls `QDBusConnection::sessionBus()`,
so the process' first session-bus connection is created by the real application — whose event loop does run — and the
delayed enable is executed normally. The Plasma applet then shows Krita's menu again (and the in-window menu bar stays
hidden, as it should).

Optionally drop the probe's accessibility connection afterwards, so the real application does not inherit a dead one:

```cpp
QDBusConnection::disconnectFromBus(QStringLiteral("a11y"));   // after the probes, before the real QApplication
```

Caveats:

* Scope it to the probe application only (Krita's `EnvironmentSetter` RAII is perfect for that). Setting it for the whole
  process also hides the symptom, but it silently disables accessibility for Krita.
* This is a workaround that relies on an implementation detail of Qt's at-spi bridge; it is not the root fix.

### Suggested fixes

* **Krita (root fix):** do not create a second `QGuiApplication` in the process — either probe inside the real application
  (`QOffscreenSurface` / hidden `QWindow` before any visible window is created, reworking the `adjustGlobalState`
  attributes), or move the probe into a **child process** (`krita --probe-opengl …`). Keep the existing portal guard;
  add the `AT_SPI_BUS_ADDRESS` guard as a stopgap until the probe no longer creates a `QGuiApplication`.
* **Qt:** make the delayed delivery enable independent of the application object that happened to exist when the connection
  was created (the queued call is currently addressed to `qApp`), and/or re-arm it in `busConnection()` whenever a cached
  connection that is still suspended is handed out. Also consider not caching a *failed* connection so permanently (`setConnection(name, d)` before
  `d->setConnection(c, error)`), since that turns "blocked bus during a probe" into "no bus at all".

### References

* KDE bug 483170 — appmenu (global menu) doesn't work with krita on plasma 6
* KDE bug 515889 — [qt6] Application Menu is unavailable on Krita 6
* KDE bug 518583 — Global menu broken on KDE Plasma after commit 2c920c28 (Remove QT_QPA_PLATFORMTHEME)
* Krita commit `8d6a2a5d12` — "Do not load the platform theme when created a test QApplication" (BUG 408015)
* Qt sources: `src/dbus/qdbusconnectionmanager.cpp`, `src/dbus/qdbusintegrator.cpp`,
  `src/gui/accessible/linux/dbusconnection.cpp`

---

## 中文

### 摘要

Krita 6（Qt 6）下，Plasma 面板的「应用程序菜单」组件始终**空白**，而 Krita 窗口内也**不画菜单栏**；
`KDE_NO_GLOBAL_MENU=1` 能让窗口内菜单栏出现。Krita 5.x（Qt 5）不受影响。

菜单其实**已经正确导出**了——真正的故障是：该进程**从不回应读取菜单的那次 D-Bus 调用**。

### Krita 侧做了什么

`KisOpenGLModeProber::probeFormat()`（`libs/ui/opengl/KisOpenGLModeProber.cpp`，由 `krita/main.cc` 经
`KisOpenGL::selectSurfaceConfig()` 调用）为了探测 OpenGL 渲染器，会在真实 `KisApplication` 创建**之前**建一个
**一次性 `QGuiApplication`**，并在其中创建 `QWindow` + `QOpenGLContext`。Krita 已经为此做了两项隔离：

* `QGuiApplication::setDesktopSettingsAware(false)`（避免在探测应用里加载 KDE 平台主题及其 `KConfigWatcher`）；
* `QT_NO_XDG_DESKTOP_PORTAL=1`（避免创建 XDG desktop portal 的设置监听）。

这在 Qt 5 上足够。Qt 6 上不够：当探测应用创建它的 `QWindow` 时，Qt 6 的
**无障碍（at-spi）桥**会打开 session bus（`src/gui/accessible/linux/dbusconnection.cpp`，
`QAtSpiDBusConnection::QAtSpiDBusConnection()`）：

```cpp
    QByteArray addressEnv = qgetenv("AT_SPI_BUS_ADDRESS");
    if (!addressEnv.isEmpty()) {                 // 非空 → 完全不碰 session bus
        m_enabled = true;
        connectA11yBus(QString::fromLocal8Bit(addressEnv));
        return;
    }
    QDBusConnection c = QDBusConnection::sessionBus();   // ← 进程级 session bus 在这里被建立
```

于是 Qt 6 下这条连接是在**一次性应用存活期间**建立的（此时 `qApp` 指向探测应用）。

### Qt 6 的行为（真正的缺陷）

1. `QDBusConnectionManager::busConnection()`（`src/dbus/qdbusconnectionmanager.cpp`）只要 `qApp` 已存在，就以
   **"先挂起投递"（suspended delivery）**模式建立标准总线连接，并把裸指针缓存下来：

   ```cpp
   bool suspendedDelivery = QThread::isMainThread() && qApp;
   ...
   if (defaultBuses[type])                       // 已缓存则直接返回，永不重新安排"恢复"
       return defaultBuses[type];
   return defaultBuses[type] = connectToBus(type, name, suspendedDelivery);
   ```

2. `connectToBus()` 把"恢复投递"这一步做成**投递给 `qApp` 的排队调用**：

   ```cpp
   if (suspendedDelivery && result && result->connection)
       result->enableDispatchDelayed(qApp);      // qApp == 那个一次性应用
   ```

3. Krita 在**任何事件循环运行之前**就销毁了这个一次性应用 → 该排队调用随接收者一起消失，
   `setDispatchEnabled(true)` 永远不执行。连接对象存活在全局 `QDBusConnectionManager` 中并被
   `defaultBuses[]` 钉住，真实应用拿到的就是同一条连接（`if (defaultBuses[type]) return defaultBuses[type];`），
   而且再也不会有人重新安排恢复：`dispatchEnabled` 永久为 `false`。

4. 因此 `QDBusConnectionPrivate::handleMessage()` 对每一个来自其它进程的调用都走这个分支：

   ```cpp
   if (!dispatchEnabled && !isLocal) {
       qDBusDebug() << this << "delivery is suspended";
       pendingMessages << amsg;
       return amsg.type() == QDBusMessage::MethodCallMessage;   // 记为"已处理"，但永不回包
   }
   ```

   调用方（Plasma 的 applet）只能超时。`QDBUS_DEBUG=1` 会对每个入站的 `Introspect`/`GetLayout`/`AboutToShow`
   打印 `delivery is suspended`。

### 为什么最终表现为"两边都没有菜单"

* Qt 的 `QDBusMenuBar` **确实**导出了菜单：在 session bus 上注册 `/MenuBar/N`，并在 xcb 下调用
  `com.canonical.AppMenu.Registrar.RegisterWindow(xid, path)`；Plasma 的 kded appmenu 模块甚至已经把
  `_KDE_NET_WM_APPMENU_SERVICE_NAME` / `_OBJECT_PATH` 写到 Krita 窗口上（这些都实测确认存在且正确）。
* 但 applet 对该对象的 `GetLayout` / `AboutToShow` 永远收不到应答 → 面板空白。
* 由于 Qt 认为存在原生（全局）菜单栏，它会**隐藏窗口内的菜单栏** → 于是完全没有菜单可用。
* `KDE_NO_GLOBAL_MENU=1` 让 `createPlatformMenuBar()` 返回 `nullptr`，Qt 就把菜单栏画回窗口内（这也是常用的绕过方式）。
* 只有连接层的 `org.freedesktop.DBus.Peer.Ping` 仍有应答（它不经过被挂起的派发路径），这一点极易误导排查——
  单看 `Ping` 会以为应用"响应正常"。

### 为什么 Qt 5 没问题

Qt 5 也有同样的"延迟恢复"设计，但在 Krita 的隔离下，探测应用根本不会建立 session bus：Qt 5 上探测期间唯一会建立它的
是 XDG portal，而它已被 `QT_NO_XDG_DESKTOP_PORTAL=1` 挡住（注：如果连接确实在一次性应用存活期间建立，Qt 5 也会以完全
相同的方式坏掉——已实测）。Qt 6 上无障碍桥也会建立它，而现有的 workaround 挡不住这一点。

### 最小复现

```cpp
// 在真实 QApplication 之前，完全照 KisOpenGLModeProber::probeFormat() 的做法：
{
    int argc = 1;
    QByteArray n("probe"); char *argv[] = { n.data(), nullptr };
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");          // Krita 已经这么做
    QGuiApplication probe(argc, argv);
    QWindow surface;
    surface.setSurfaceType(QSurface::OpenGLSurface);
    surface.create();                                   // ← 就是这一步让 Qt 6 建立 session bus
}
QApplication app(argc, argv);
QMainWindow w; w.menuBar()->addMenu("File")->addAction("Open"); w.show();
```

再从第二个进程查询它导出的对象：

```sh
gdbus call --session --dest <krita-或探测程序的-bus-name> --object-path /MenuBar/N \
      --method com.canonical.dbusmenu.GetLayout 0 1 "@as []"     # → 无应答 / 超时
gdbus call --session --dest <krita-或探测程序的-bus-name> --object-path /MenuBar/N \
      --method com.canonical.dbusmenu.AboutToShow 0             # → 无应答 / 超时
gdbus call --session --dest <krita-或探测程序的-bus-name> --object-path / \
      --method org.freedesktop.DBus.Peer.Ping                   # → 有应答
```

完整的**带窗口**最小复现（Qt 5 + Qt 6，CMake）就在本仓库（`appmenu-test`）里：
`MODE = plain | core | probe | krita`，其中 `probe`/`krita` 会创建一次性 `QGuiApplication`/`QWindow`。

用它的实测结果（Qt 6.11.2；数值为跨进程 `Introspect` / `AboutToShow` 的退出码，`124` = 超时）：

| 配置 | `isNativeMenuBar` | `/MenuBar/1` 已注册 | 跨进程探针 |
|---|---|---|---|
| `plain`（对照） | 1 | 1 | 0 / 0 |
| `probe`（只建一次性 `QGuiApplication`，不建窗口） | 1 | 1 | 0 / 0 |
| **`probe`（一次性应用创建 `QWindow`，即 Krita 同构）** | 1 | 1 | **124 / 124（复现）** |
| `probe` + 探测期间把 `DBUS_SESSION_BUS_ADDRESS` 指向死地址 | **0** | **0** | 无对象（整条 session bus 死掉） |
| `probe` + 探测后 `disconnectFromBus("qt_default_session_bus")` | 1 | 1 | 124 / 124（无效） |
| **`probe` + 探测期间 `AT_SPI_BUS_ADDRESS=1`** | 1 | 1 | **0 / 0（修好）** |
| Qt 5，以上全部配置 | 1 | 1 | 0 / 0 |

### 缓解办法（workaround）

**在一次性探测应用存活期间，把 `AT_SPI_BUS_ADDRESS` 设为任意非空值（例如 `AT_SPI_BUS_ADDRESS=1`）。**
Qt 的无障碍桥随即走上面那段 early-return，不再调用 `QDBusConnection::sessionBus()`；于是进程的第一条 session bus
连接由**真实应用**建立（它的事件循环会真正运行），延迟恢复得以正常执行，Plasma 面板重新显示 Krita 的菜单
（窗口内菜单栏照旧隐藏，这是预期行为）。

可选：探测结束后把探测应用留下的无障碍连接清掉，避免真实应用继承一条死连接：

```cpp
QDBusConnection::disconnectFromBus(QStringLiteral("a11y"));   // 探测结束、真实 QApplication 创建之前
```

注意事项：

* 只在探测应用存活期间设置（Krita 现成的 `EnvironmentSetter` RAII 正合适）。全程设置也能遮住现象，但会静默地
  让整个 Krita 进程失去无障碍支持。
* 这依赖 Qt 无障碍桥"`AT_SPI_BUS_ADDRESS` 非空即 early-return"的实现细节，属于缓解而非根治。

### 建议的修法

* **Krita（根治）**：不要在进程里创建第二个 `QGuiApplication` —— 要么把探测搬进真实应用（任何可见窗口创建之前用
  `QOffscreenSurface` / 隐藏 `QWindow`，并重构 `adjustGlobalState` 那几个属性），要么把探测放进**子进程**
  （`krita --probe-opengl …`）。保留现有的 portal guard；在探测不再创建 `QGuiApplication` 之前，先加上
  `AT_SPI_BUS_ADDRESS` 这道保险。
* **Qt**：让"延迟恢复投递"这一步不依赖"建立连接时恰好存在的那个应用对象"（目前这个排队调用是投给 `qApp` 的）；
  或者至少让 `busConnection()` 在交出一条仍是 suspended 的缓存连接时，重新安排一次延迟恢复。另外建议不要把**失败**的连接也永久
  缓存（`setConnection(name, d)` 出现在 `d->setConnection(c, error)` 之前），否则"探测期间屏蔽总线"会变成"整进程
  没有总线"。

### 相关记录

* KDE bug 483170 — appmenu (global menu) doesn't work with krita on plasma 6
* KDE bug 515889 — [qt6] Application Menu is unavailable on Krita 6
* KDE bug 518583 — Global menu broken on KDE Plasma after commit 2c920c28 (Remove QT_QPA_PLATFORMTHEME)
* Krita 提交 `8d6a2a5d12` — "Do not load the platform theme when created a test QApplication"（BUG 408015）
* Qt 源文件：`src/dbus/qdbusconnectionmanager.cpp`、`src/dbus/qdbusintegrator.cpp`、
  `src/gui/accessible/linux/dbusconnection.cpp`
