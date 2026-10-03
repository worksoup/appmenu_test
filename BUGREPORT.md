# Krita 6 (Qt 6): Plasma global menu stays empty — the session-bus connection belongs to a throw-away QApplication created during OpenGL probing

**Product / component:** krita / general (Qt 6 builds)
**Version:** 6.0.4 (distro) and master `351297cdce` (6.1.0-prealpha)
**Environment:** Arch Linux, KDE Plasma 6, Wayland session with Krita on XWayland (`QT_QPA_PLATFORM=xcb`), Qt **6.11.2**,
KF6 6.30.0, `plasma-integration` 6.7.5, Qt 5.15.19 for comparison
**Reported:** 2026-10-03 · root-cause analysis and minimal reproducer by the **DeepSeek Harness coding agent** with the reporter

---

## 1. Symptom

* The Plasma *Application Menu* widget stays **empty** while Krita is focused.
* Krita's own menu bar is **not drawn inside the window** either, so there is no menu at all.
* `KDE_NO_GLOBAL_MENU=1` makes the in-window menu bar appear (the classic workaround).
* Krita 5.x (Qt 5 build) is unaffected.

The menu is exported correctly — `/MenuBar/N` is registered and Plasma's appmenu module writes
`_KDE_NET_WM_APPMENU_SERVICE_NAME` / `_OBJECT_PATH` onto Krita's window. What fails is that Krita
**never replies** to the calls that read that object.

## 2. Root cause in one paragraph

Before Krita's real application object exists, `KisOpenGLModeProber` creates a **throw-away `QGuiApplication`**
(plus a `QWindow` and a `QOpenGLContext`) to probe the OpenGL renderers. In Qt 6, **destroying that probe's
`QWindow` calls `QAccessible::isActive()`** — a check that did not exist in Qt 5.15's `QWindow` destructor — and
that call ends up instantiating the platform plugin's accessibility implementation (the at-spi bridge), which
**opens the process-wide session-bus connection while the throw-away application is still the current `qApp`**.
Qt's D-Bus connection is a **process-level** resource and the step that re-enables message delivery was queued
**on that throw-away `qApp`**; when the throw-away application is destroyed that queued step dies with it, and
the cached connection is handed to the real application **without being re-armed**. From then on the process
never answers incoming D-Bus method calls, so the Plasma applet times out.

Note that the probing code **already excludes most of the things that would otherwise create a D-Bus connection**
(see §3.1): the KDE platform theme and the XDG-desktop-portal watcher are both switched off for the probe. The
Qt 6 destructor-time accessibility query is the one producer that no existing guard covers.

## 3. The chain, step by step

### 3.1 Krita creates a complete, short-lived `QGuiApplication` before the real one

`krita/main.cc:442` → `KisOpenGL::selectSurfaceConfig()` (`libs/ui/opengl/kis_opengl.cpp:910`) →
`KisOpenGLModeProber::probeFormat(config, adjustGlobalState = true)` (`libs/ui/opengl/KisOpenGLModeProber.cpp`).
That function already takes care to keep the probe application off D-Bus:

```cpp
// libs/ui/opengl/KisOpenGLModeProber.cpp  (abridged)
QScopedPointer<EnvironmentSetter> portalSetter;                        // ~line 180
portalSetter.reset(new EnvironmentSetter(QLatin1String("QT_NO_XDG_DESKTOP_PORTAL"), QLatin1String("1")));
...
if (runningInKDE && !isInAppimage)
    QGuiApplication::setDesktopSettingsAware(false);                   // ~line 188-190
...
QScopedPointer<QGuiApplication> application(new QGuiApplication(argc, &argv));   // ~line 195
...
QWindow surface;                                                       // ~line 203
surface.setFormat(format);
surface.setSurfaceType(QSurface::OpenGLSurface);
surface.create();
QOpenGLContext context; ...                                            // GL probe
// ... function returns here: the QWindow and then the QGuiApplication are destroyed
```

* `QT_NO_XDG_DESKTOP_PORTAL=1` prevents the portal-based settings watcher (`QDesktopUnixServices`) from opening
  the session bus while the probe is alive.
* `setDesktopSettingsAware(false)` prevents the KDE platform theme (and its `KIconLoader` / `KConfigWatcher`
  D-Bus usage) from being created inside the probe.

The separate temporary `QCoreApplication` in `krita/main.cc:377` is measured to be harmless (a core application
alone never creates a D-Bus connection).

### 3.2 Qt 6 added an accessibility query to `QWindow`'s destructor

```cpp
// qtbase/src/gui/kernel/qwindow.cpp:178-181 (v6.11.2)
QWindow::~QWindow()
{
    Q_D(QWindow);
#if QT_CONFIG(accessibility)
    if (QGuiApplicationPrivate::is_app_running && !QGuiApplicationPrivate::is_app_closing && QAccessible::isActive())
        QAccessibleCache::instance()->sendObjectDestroyedEvent(this);
#endif
```

Qt 5.15's destructor contains **no accessibility code at all**:

```cpp
// qtbase/src/gui/kernel/qwindow.cpp (v5.15)
QWindow::~QWindow()
{
    Q_D(QWindow);
    d->destroy();
    QGuiApplicationPrivate::window_list.removeAll(this);
    ...
}
```

The intent of the Qt 6 code is correct and cheap-looking: before announcing that a window's accessible object
disappeared, ask whether accessibility is active at all (the documentation of `QAccessible::isActive()`
explicitly recommends it as a guard "to prevent expensive notifications"). The problem is that this "cheap
guard" is not cheap: `QAccessible::isActive()` has to obtain the platform accessibility object first.

In the probe application nothing else ever touches accessibility: a bare `QWindow` has exactly four
accessibility-related call sites in Qt 6 (`~QWindow` = ObjectDestroyed, `QWindow::setParent` = ParentChanged,
`QWindow::event()` FocusIn/FocusOut = state changes), and no creation/show event is sent for a `QWindow` at all.
The probe's window is never shown and never focused, so **its destruction is the first — and only — point at
which the probe application ever asks for accessibility**.

### 3.3 That query instantiates the at-spi bridge and opens the session bus

```cpp
// src/gui/accessible/qaccessible.cpp
static QPlatformAccessibility *platformAccessibility()
{
    QPlatformIntegration *pfIntegration = QGuiApplicationPrivate::platformIntegration();
    return pfIntegration ? pfIntegration->accessibility() : nullptr;      // virtual -> QPA plugin
}
bool QAccessible::isActive()
{
    if (QPlatformAccessibility *pfAccessibility = platformAccessibility())
        return pfAccessibility->isActive();
    return false;
}
```

```cpp
// src/plugins/platforms/xcb/qxcbintegration.cpp:398-409
QPlatformAccessibility *QXcbIntegration::accessibility() const
{
    if (!m_accessibility) {
        Q_ASSERT_X(QCoreApplication::eventDispatcher(), "QXcbIntegration", ...);
        m_accessibility.reset(new QSpiAccessibleBridge());                // ← lazy construction
    }
    return m_accessibility.data();
}
```

```cpp
// src/plugins/platforms/wayland/qwaylandintegration.cpp:256-270 — same pattern
QPlatformAccessibility *QWaylandIntegration::accessibility() const
{
    if (!mAccessibility) {
        Q_ASSERT_X(QCoreApplication::eventDispatcher(), "QWaylandIntegration", ...);
        mAccessibility.reset(new QSpiAccessibleBridge());
    }
    return mAccessibility.data();
}
```

`QSpiAccessibleBridge`'s constructor creates `QAtSpiDBusConnection`, and that one opens the bus
(`src/gui/accessible/linux/dbusconnection.cpp`):

```cpp
QAtSpiDBusConnection::QAtSpiDBusConnection(QObject *parent) : ... {
    QByteArray addressEnv = qgetenv("AT_SPI_BUS_ADDRESS");
    if (!addressEnv.isEmpty()) {                 // non-empty -> never touches the session bus
        m_enabled = true;
        connectA11yBus(QString::fromLocal8Bit(addressEnv));
        return;
    }
    QDBusConnection c = QDBusConnection::sessionBus();   // ← the process-wide session bus is created here
    ...
```

Confirmed with gdb (breakpoint on `QDBusConnection::sessionBus()`, first hit during the probe):

```
#0 QDBusConnection::sessionBus()                      libQt6DBus
#1 ??                                                 libQt6Gui   (= QAtSpiDBusConnection ctor)
#2 QSpiAccessibleBridge::QSpiAccessibleBridge()       libQt6Gui
#3 QXcbIntegration::accessibility() const              libQt6XcbQpa
#4 QAccessible::isActive()                             libQt6Gui
#5 QWindow::~QWindow()                                 libQt6Gui   ← the trigger
#6 KisOpenGLModeProber-style probe iteration
#7 main()
```

### 3.4 Why the throw-away application's death breaks the real application

**(a) The connection is process-level, not application-level.** It is owned by a global singleton
(`Q_GLOBAL_STATIC(QDBusConnectionManager, _q_manager)`), kept in `connectionHash` plus the raw
`defaultBuses[type]` pointer, and held alive by `QDBusConnectionPrivate::ref` (initial value 1). Deleting the
probe's platform theme and platform integration happens in `QGuiApplicationPrivate::cleanup()`
(`delete platform_theme; delete platform_integration;` in `qguiapplication.cpp:1886-1891`) — that only drops
the *users* of the connection; QtDBus has no per-application teardown, and the global manager is destroyed only
at process exit (and even then its destructor merely stops its thread).

**(b) The "delivery will be re-enabled later" step was bound to the `qApp` that existed at creation time.**

```cpp
// src/dbus/qdbusconnectionmanager.cpp
bool suspendedDelivery = QThread::isMainThread() && qApp;          // true → qApp == the probe app
...
if (suspendedDelivery && result && result->connection)
    result->enableDispatchDelayed(qApp);                            // queued invocation on the probe app
```

```cpp
// src/dbus/qdbusintegrator.cpp
void QDBusConnectionPrivate::enableDispatchDelayed(QObject *context)
{
    ref.ref();
    QMetaObject::invokeMethod(context, [this]() {                   // receiver == that qApp
        QMetaObject::invokeMethod(this, &QDBusConnectionPrivate::setDispatchEnabled, Qt::QueuedConnection, true);
        ...
    }, Qt::QueuedConnection);
}
```

The probe application is destroyed before any event loop runs, so this queued invocation is dropped together
with its receiver and `setDispatchEnabled(true)` is never executed.

**(c) The cached connection is never re-associated with the new application.** Every later
`QDBusConnection::sessionBus()` — including the real `KisApplication`'s — takes this early return:

```cpp
if (defaultBuses[type])
    return defaultBuses[type];          // cached pointer; the new qApp never re-arms the delayed enable
```

`defaultBuses[]` is only initialised (to `nullptr`) in the manager's constructor and is never reset;
`QDBusConnection::disconnectFromBus("qt_default_session_bus")` cannot clear it either
(`removeConnection()` only removes the `connectionHash` entry). So the connection object keeps
`dispatchEnabled == false` for the rest of the process, and the association with the destroyed `qApp` is
effectively stale forever.

**(d) The observable result:**

```cpp
// src/dbus/qdbusintegrator.cpp — QDBusConnectionPrivate::handleMessage()
if (!dispatchEnabled && !isLocal) {
    qDBusDebug() << this << "delivery is suspended";
    pendingMessages << amsg;
    return amsg.type() == QDBusMessage::MethodCallMessage;   // reported as handled, no reply is ever sent
}
```

Every incoming method call is queued and never answered (and no error is returned either). Only the
connection-level `org.freedesktop.DBus.Peer.Ping` is answered, because it does not go through this gate —
which makes the failure easy to misdiagnose.

**(e) Why the user-visible symptoms are "empty applet **and** no in-window menu bar":** outgoing/blocking calls
work normally, so `RegisterWindow` succeeds and Plasma even stores `(:service, /MenuBar/N)` for Krita's
window; but the applet's `GetLayout`/`AboutToShow` on that object never get a reply and time out. Since Qt
believes a native global menu bar exists, it also hides the in-window menu bar. `KDE_NO_GLOBAL_MENU=1` makes
`createPlatformMenuBar()` return `nullptr`, so Qt draws the menu bar inside the window instead.

## 4. Evidence

* **Cross-process probes** against Krita's exported menu object: `Introspect`, `GetLayout`, `AboutToShow`
  time out; `org.freedesktop.DBus.Peer.Ping` answers. With `QDBUS_DEBUG=1` Krita itself prints
  `delivery is suspended` for each of those calls.
* **The menu is registered correctly** — `xprop` shows `_KDE_NET_WM_APPMENU_SERVICE_NAME` / `_OBJECT_PATH`, and
  the registrar returns `(':1.x', '/MenuBar/N')` — so this is not a registration or KWin problem.
* **gdb** backtrace of the first `QDBusConnection::sessionBus()` call inside the probe application (§3.3).
* **Bisection with a minimal reproducer** (a windowed Qt 5/Qt 6 program that optionally creates a throw-away
  `QGuiApplication`/`QWindow` before the real `QApplication`):

  | configuration | `QMenuBar::isNativeMenuBar()` | `/MenuBar/1` registered | cross-process `Introspect`/`AboutToShow` |
  |---|---|---|---|
  | plain application | true | true | 0 / 0 |
  | throw-away `QCoreApplication` only | true | true | 0 / 0 |
  | throw-away `QGuiApplication` without a `QWindow` | true | true | 0 / 0 |
  | **throw-away `QGuiApplication` + `QWindow` (Krita's prober)** | true | true | **124 / 124 (timeout)** |
  | same, but `AT_SPI_BUS_ADDRESS` non-empty during the probe | true | true | **0 / 0** |
  | same, but `DBUS_SESSION_BUS_ADDRESS` poisoned during the probe | false | false | object not registered at all |

  (`124` = the call timed out without a reply.)
  Qt 5 is healthy in every one of these configurations *except* when the probe really opens the session bus
  (which is what `QT_NO_XDG_DESKTOP_PORTAL=1` prevents in Krita's Qt 5 build).

## 5. Workaround

**Set `AT_SPI_BUS_ADDRESS` to any non-empty value while the throw-away probe application is alive** (Krita's
existing `EnvironmentSetter` RAII pattern is a natural fit), and optionally drop the probe's accessibility
connection before the real application is created:

```cpp
// in KisOpenGLModeProber::probeFormat(), next to the existing portal guard
a11ySetter.reset(new EnvironmentSetter(QLatin1String("AT_SPI_BUS_ADDRESS"),
                                       QLatin1String("unix:path=/nonexistent-krita-probe")));
...
// after the probe application(s) are gone, before KisApplication is constructed
QDBusConnection::disconnectFromBus(QStringLiteral("a11y"));
```

Because the at-spi bridge takes the early-return path shown in §3.3, `QDBusConnection::sessionBus()` is not
called from the probe at all; the process' first session-bus connection is then created by the real application,
whose event loop does run, so the delayed delivery enable executes normally and the applet displays Krita's menu.
Measured: with this guard the exported object answers immediately, and `KDE_NO_GLOBAL_MENU` is no longer needed.

Caveats: keep it scoped to the probe application — setting it for the whole process also hides the problem but
silently disables accessibility for Krita. `disconnectFromBus("a11y")` is needed because the probe's bridge has
already created a named connection under that address. Neither the KDE platform theme guard nor the portal
guard can replace this one; both must stay.

## 6. Suggested fixes

* **Krita (root fix):** do not create a second `QGuiApplication` in the process. Either probe inside the real
  application (e.g. with a `QOffscreenSurface`/hidden `QWindow` before any visible window exists, reworking the
  `Qt::AA_*` attributes that `probeFormat(adjustGlobalState = true)` currently sets before application
  construction), or move the probing into a **child process** (`krita --probe-opengl …`). Until then, keep the
  existing guards and add the `AT_SPI_BUS_ADDRESS` guard from §5 as a stopgap.
* **Qt (upstream):** make the delayed delivery enable independent of the application object that happened to
  exist when the connection was created, and/or re-arm it when `QDBusConnectionManager::busConnection()` hands
  out a cached connection that is still suspended. Additionally, do not cache a *failed* connection
  unconditionally in `defaultBuses[]` (a separate upstream report covers both points).

## 7. Related reports

* KDE bug 483170 — appmenu (global menu) doesn't work with krita on plasma 6
* KDE bug 515889 — [qt6] Application Menu is unavailable on Krita 6
* KDE bug 518583 — Global menu broken on KDE Plasma after commit 2c920c28 (Remove QT_QPA_PLATFORMTHEME)
* Krita commit `8d6a2a5d12` — "Do not load the platform theme when created a test QApplication" (BUG 408015);
  i.e. the project has already had to patch the probe application for exactly this class of problem.

---

# 中文版

## 一、现象

* Krita 获得焦点时，Plasma 的「应用程序菜单」组件（全局菜单）**始终空白**。
* Krita 窗口内**也没有菜单栏**，也就是完全没有菜单可用。
* 设 `KDE_NO_GLOBAL_MENU=1` 可以让窗口内菜单栏出现（经典绕过方式）。
* Krita 5.x（Qt 5 构建）没有此问题。

菜单其实**已经正确导出**：`/MenuBar/N` 已注册，Plasma 的 appmenu 模块也把
`_KDE_NET_WM_APPMENU_SERVICE_NAME` / `_OBJECT_PATH` 写到了 Krita 窗口上。真正失效的是——
Krita **从不回应**读取该对象的那些 D-Bus 调用。

## 二、根因（一段话）

在 Krita 的真实应用对象存在之前，`KisOpenGLModeProber` 会为了探测 OpenGL 渲染器创建一个**一次性
`QGuiApplication`**（以及一个 `QWindow` 和一个 `QOpenGLContext`）。在 Qt 6 中，**销毁该探测窗口时会调用
`QAccessible::isActive()`**（Qt 5.15 的 `QWindow` 析构函数里没有这个判断），这次调用最终会实例化平台插件的
无障碍实现（at-spi 桥），从而**在那个一次性应用仍是 `qApp` 的时候，打开了进程级的 session bus 连接**。
Qt 的 D-Bus 连接是**进程级**资源，而"恢复投递"这一步是**排队投递给当时那个一次性 `qApp`** 的；一次性应用
销毁后这个排队调用随之消失，此后缓存下来的连接被交给真实应用时**不会重新绑定、也不会重新安排恢复**，
于是进程再也不回应任何入站的 D-Bus 方法调用，Plasma 的 applet 只能超时。

另外要强调的是：**这段探测代码本身已经排除了大部分会产生 D-Bus 连接的功能**（见 3.1 节）——KDE 平台主题和
XDG desktop portal 的监听都被关掉了。Qt 6 新增的"析构时查询无障碍"是所有现存 guard 都没能覆盖的那一个来源。

## 三、调用链

### 3.1 Krita 在真实应用之前创建了一个完整而短命的应用对象

`krita/main.cc:442` → `KisOpenGL::selectSurfaceConfig()`（`libs/ui/opengl/kis_opengl.cpp:910`）→
`KisOpenGLModeProber::probeFormat(config, adjustGlobalState = true)`（`libs/ui/opengl/KisOpenGLModeProber.cpp`）。
它已经主动避免让探测应用接触 D-Bus：

```cpp
// libs/ui/opengl/KisOpenGLModeProber.cpp（节选）
QScopedPointer<EnvironmentSetter> portalSetter;                       // ~180 行
portalSetter.reset(new EnvironmentSetter(QLatin1String("QT_NO_XDG_DESKTOP_PORTAL"), QLatin1String("1")));
...
if (runningInKDE && !isInAppimage)
    QGuiApplication::setDesktopSettingsAware(false);                  // ~188-190 行
...
QScopedPointer<QGuiApplication> application(new QGuiApplication(argc, &argv));   // ~195 行
...
QWindow surface; ... surface.create();                                // ~203-206 行
QOpenGLContext context; ...                                           // GL 探测
// 函数返回：QWindow 先析构，随后 QGuiApplication 析构
```

* `QT_NO_XDG_DESKTOP_PORTAL=1` 阻止基于 portal 的设置监听（`QDesktopUnixServices`）在探测期间打开 session bus；
* `setDesktopSettingsAware(false)` 阻止在探测应用里创建 KDE 平台主题（及其 `KIconLoader` / `KConfigWatcher`
  的 D-Bus 使用）。

`krita/main.cc:377` 那个临时 `QCoreApplication` 实测无害（只有 core 应用时不会建立 D-Bus 连接）。

### 3.2 Qt 6 给 `QWindow` 析构函数加了无障碍查询

```cpp
// qtbase/src/gui/kernel/qwindow.cpp:178-181 (v6.11.2)
QWindow::~QWindow()
{
    Q_D(QWindow);
#if QT_CONFIG(accessibility)
    if (QGuiApplicationPrivate::is_app_running && !QGuiApplicationPrivate::is_app_closing && QAccessible::isActive())
        QAccessibleCache::instance()->sendObjectDestroyedEvent(this);
#endif
```

Qt 5.15 的析构函数里**完全没有无障碍代码**：

```cpp
// qtbase/src/gui/kernel/qwindow.cpp (v5.15)
QWindow::~QWindow()
{
    Q_D(QWindow);
    d->destroy();
    QGuiApplicationPrivate::window_list.removeAll(this);
    ...
}
```

Qt 6 这段代码的意图是正确的：在宣告"某个窗口的可访问对象消失了"之前，先判断无障碍是否启用
（`QAccessible::isActive()` 的文档正是建议用它来"避免不必要的昂贵通知"）。问题在于——**这个"廉价守卫"并不廉价**：
`QAccessible::isActive()` 必须先向平台插件索要无障碍实现。

而在探测应用里，其它任何地方都不会碰无障碍：Qt 6 中一个裸 `QWindow` 与无障碍相关的调用点只有 4 处
（`~QWindow` 的 ObjectDestroyed、`QWindow::setParent` 的 ParentChanged、`QWindow::event()` 的
FocusIn/FocusOut），且 `QWindow` **根本不发送创建/显示类事件**。探测窗口从未 show、从未获得焦点，
因此**它的析构就是探测应用整个生命里第一次、也是唯一一次索要无障碍实现**。

### 3.3 这次查询会实例化 at-spi 桥并打开 session bus

```cpp
// src/gui/accessible/qaccessible.cpp
static QPlatformAccessibility *platformAccessibility()
{
    QPlatformIntegration *pfIntegration = QGuiApplicationPrivate::platformIntegration();
    return pfIntegration ? pfIntegration->accessibility() : nullptr;      // 虚函数 → QPA 插件
}
bool QAccessible::isActive()
{
    if (QPlatformAccessibility *pfAccessibility = platformAccessibility())
        return pfAccessibility->isActive();
    return false;
}
```

```cpp
// src/plugins/platforms/xcb/qxcbintegration.cpp:398-409
QPlatformAccessibility *QXcbIntegration::accessibility() const
{
    if (!m_accessibility) {
        Q_ASSERT_X(QCoreApplication::eventDispatcher(), "QXcbIntegration", ...);
        m_accessibility.reset(new QSpiAccessibleBridge());                // ← 懒加载构造
    }
    return m_accessibility.data();
}
```

```cpp
// src/plugins/platforms/wayland/qwaylandintegration.cpp:256-270 —— 同样的写法
QPlatformAccessibility *QWaylandIntegration::accessibility() const
{
    if (!mAccessibility) {
        Q_ASSERT_X(QCoreApplication::eventDispatcher(), "QWaylandIntegration", ...);
        mAccessibility.reset(new QSpiAccessibleBridge());
    }
    return mAccessibility.data();
}
```

`QSpiAccessibleBridge` 的构造函数会创建 `QAtSpiDBusConnection`，后者打开总线
（`src/gui/accessible/linux/dbusconnection.cpp`）：

```cpp
QAtSpiDBusConnection::QAtSpiDBusConnection(QObject *parent) : ... {
    QByteArray addressEnv = qgetenv("AT_SPI_BUS_ADDRESS");
    if (!addressEnv.isEmpty()) {                 // 非空则完全不碰 session bus
        m_enabled = true;
        connectA11yBus(QString::fromLocal8Bit(addressEnv));
        return;
    }
    QDBusConnection c = QDBusConnection::sessionBus();   // ← 进程级 session bus 在这里被建立
    ...
```

gdb 实证（在 `QDBusConnection::sessionBus()` 上打断点，探测期间的第一次命中）：

```
#0 QDBusConnection::sessionBus()                      libQt6DBus
#1 ??                                                 libQt6Gui   （即 QAtSpiDBusConnection 构造）
#2 QSpiAccessibleBridge::QSpiAccessibleBridge()       libQt6Gui
#3 QXcbIntegration::accessibility() const              libQt6XcbQpa
#4 QAccessible::isActive()                             libQt6Gui
#5 QWindow::~QWindow()                                 libQt6Gui   ← 触发点
#6 探测迭代（同 KisOpenGLModeProber 的结构）
#7 main()
```

### 3.4 为什么"一次性应用死亡"会毁掉真实应用的连接

**（a）连接是进程级的，不属于任何应用对象。** 它由全局单例持有
（`Q_GLOBAL_STATIC(QDBusConnectionManager, _q_manager)`），放在 `connectionHash` 里，并由裸指针
`defaultBuses[type]` 缓存，靠 `QDBusConnectionPrivate::ref`（初值 1）活着。探测应用的
`QGuiApplication` 析构只会走 `QGuiApplicationPrivate::cleanup()`
（`delete platform_theme; delete platform_integration;`，`qguiapplication.cpp:1886-1891`），也就是只销毁这条
连接的**使用者**；QtDBus 没有任何"随应用销毁而清理"的逻辑，全局 manager 直到进程退出才销毁
（而它的析构也只是停掉自己的线程）。

**（b）"稍后恢复投递"这一步绑定在创建连接时存在的那个 `qApp` 上。**

```cpp
// src/dbus/qdbusconnectionmanager.cpp
bool suspendedDelivery = QThread::isMainThread() && qApp;     // true → qApp 就是那个探测应用
...
if (suspendedDelivery && result && result->connection)
    result->enableDispatchDelayed(qApp);                      // 排队调用投给探测应用
```

探测应用在任何事件循环运行之前就被销毁 → 这个排队调用随接收者一起消失 →
`setDispatchEnabled(true)` 永远不会执行。

**（c）缓存下来的连接不会被重新关联到新的应用对象。** 之后每一次
`QDBusConnection::sessionBus()`（包括真实 `KisApplication` 的调用）都会命中这个早退：

```cpp
if (defaultBuses[type])
    return defaultBuses[type];      // 直接返回缓存指针；新的 qApp 不会重新安排"延迟恢复"
```

`defaultBuses[]` 只在 manager 构造函数里初始化（为 `nullptr`），此后永不重置；
`QDBusConnection::disconnectFromBus("qt_default_session_bus")` 也清不掉它
（`removeConnection()` 只删 `connectionHash` 里的条目）。于是这条连接在整个进程余下的生命周期里
始终 `dispatchEnabled == false`，而它与那个已销毁 `qApp` 的关联也就**永远是失效的**。

**（d）可观察结果：**

```cpp
// src/dbus/qdbusintegrator.cpp — QDBusConnectionPrivate::handleMessage()
if (!dispatchEnabled && !isLocal) {
    qDBusDebug() << this << "delivery is suspended";
    pendingMessages << amsg;
    return amsg.type() == QDBusMessage::MethodCallMessage;   // 记为"已处理"，但永不回包
}
```

所有入站方法调用都被入队且永不回应（连错误都不返回）。只有连接层的
`org.freedesktop.DBus.Peer.Ping` 仍会应答（它不经过这个闸门），这也是它容易被误诊的原因。

**（e）为什么表现为"面板空 **且** 窗口内没有菜单栏"：** 外呼和阻塞式调用都是正常的，所以
`RegisterWindow` 成功、Plasma 甚至记住了 `(:service, /MenuBar/N)`；但 applet 对该对象的
`GetLayout`/`AboutToShow` 永远收不到应答而超时。同时 Qt 认为存在原生全局菜单栏，于是把窗口内菜单栏隐藏了。
`KDE_NO_GLOBAL_MENU=1` 会让 `createPlatformMenuBar()` 返回 `nullptr`，Qt 就把菜单栏画回窗口内。

## 四、证据

* **跨进程探针**：对 Krita 导出的菜单对象做 `Introspect` / `GetLayout` / `AboutToShow` 全部超时；
  `org.freedesktop.DBus.Peer.Ping` 有应答。开 `QDBUS_DEBUG=1` 时 Krita 自己对每个调用打印
  `delivery is suspended`。
* **菜单确实注册成功**：`xprop` 能看到 `_KDE_NET_WM_APPMENU_SERVICE_NAME` / `_OBJECT_PATH`，registrar 也能
  返回 `(':1.x', '/MenuBar/N')`——所以这不是注册问题，也不是 KWin 问题。
* **gdb 调用栈**：探测应用里第一次 `QDBusConnection::sessionBus()` 的完整栈（见 3.3）。
* **最小复现器二分**（一个带窗口的 Qt 5 / Qt 6 程序，可在真实 `QApplication` 之前选择性创建一次性
  `QGuiApplication`/`QWindow`）：

  | 配置 | `isNativeMenuBar()` | `/MenuBar/1` 注册 | 跨进程 `Introspect`/`AboutToShow` |
  |---|---|---|---|
  | 普通应用 | true | true | 0 / 0 |
  | 只创建临时 `QCoreApplication` | true | true | 0 / 0 |
  | 临时 `QGuiApplication`，不建 `QWindow` | true | true | 0 / 0 |
  | **临时 `QGuiApplication` + `QWindow`（即 Krita prober）** | true | true | **124 / 124（超时）** |
  | 同上，但探测期间 `AT_SPI_BUS_ADDRESS` 非空 | true | true | **0 / 0** |
  | 同上，但探测期间把 `DBUS_SESSION_BUS_ADDRESS` 指向死地址 | false | false | 对象根本没注册 |

  （`124` = 调用超时无应答。）Qt 5 在上述所有配置下都正常，**唯一例外**是探测应用真的打开了 session bus 时
  （也就是 Krita 的 Qt 5 构建用 `QT_NO_XDG_DESKTOP_PORTAL=1` 挡住的那条路径）。

## 五、缓解办法

**在一次性探测应用存活期间，把 `AT_SPI_BUS_ADDRESS` 设为任意非空值**（正好可以用 Krita 现成的
`EnvironmentSetter` RAII 模式），并在真实应用创建之前把探测应用留下的无障碍连接清掉：

```cpp
// KisOpenGLModeProber::probeFormat() 里，与现有 portal guard 并列
a11ySetter.reset(new EnvironmentSetter(QLatin1String("AT_SPI_BUS_ADDRESS"),
                                       QLatin1String("unix:path=/nonexistent-krita-probe")));
...
// 所有探测应用销毁之后、KisApplication 构造之前
QDBusConnection::disconnectFromBus(QStringLiteral("a11y"));
```

由于 at-spi 桥会走 3.3 节那段 early-return，探测期间**根本不会调用 `QDBusConnection::sessionBus()`**；
进程的第一条 session bus 连接由真实应用建立，而它的事件循环会真正运行，"延迟恢复投递"得以正常执行，
面板就会显示 Krita 的菜单。实测：加上这道保险后，导出的对象立即有应答，而且不再需要
`KDE_NO_GLOBAL_MENU`。

注意事项：只应作用在探测应用期间（全局设置同样能遮住现象，但会静默地让整个 Krita 失去无障碍支持）；
`disconnectFromBus("a11y")` 是必要的，因为探测应用的桥已经在那个地址名下建了一条命名连接；
这道保险**不能**被 KDE 平台主题 guard 或 portal guard 替代，两者都必须保留。

## 六、建议的修法

* **Krita（根治）**：不要在进程里创建第二个 `QGuiApplication`。要么把探测搬进真实应用（在任何可见窗口
  创建之前用 `QOffscreenSurface`/隐藏 `QWindow`，并重构 `probeFormat(adjustGlobalState = true)` 目前在
  应用构造前设置的 `Qt::AA_*` 属性），要么把探测放进**子进程**（`krita --probe-opengl …`）。在完成之前，
  保留现有 guard，并加上第五节的 `AT_SPI_BUS_ADDRESS` 保险作为过渡。
* **Qt（上游）**：让"延迟恢复投递"不依赖建立连接时恰好存在的那个应用对象；或者在
  `QDBusConnectionManager::busConnection()` 交出仍是 suspended 的缓存连接时重新安排一次恢复。另外不要把
  **失败**的连接也无条件缓存进 `defaultBuses[]`（这两点另有一份上游报告详述）。

## 七、相关报告

* KDE bug 483170 — appmenu (global menu) doesn't work with krita on plasma 6
* KDE bug 515889 — [qt6] Application Menu is unavailable on Krita 6
* KDE bug 518583 — Global menu broken on KDE Plasma after commit 2c920c28 (Remove QT_QPA_PLATFORMTHEME)
* Krita 提交 `8d6a2a5d12` — "Do not load the platform theme when created a test QApplication"（BUG 408015），
  也就是说这个项目此前已经为同一类问题修补过探测应用。
