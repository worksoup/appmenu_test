# 原因分析：Krita 6（Qt 6）下 Plasma 全局菜单空白

经 DeepSeek V4.1 Flash 分析找出原因，并给出最小复现以供验证。

**环境：** Arch Linux、KDE Plasma 6（Wayland 会话，Krita 运行于 XWayland，`QT_QPA_PLATFORM=xcb`）、Qt **6.11.2**、
KF6 6.30.0、`plasma-integration` 6.7.5；Krita master `351297cdce`（6.1.0-prealpha）与 6.0.4；对照 Qt 5.15.19

**报告时间：** 2026-10-03

## 一、概述

此部分给出简要分析及缓解措施。

### 1.1 根因

在 Krita 的真实应用对象存在之前，`KisOpenGLModeProber` 会为了探测 OpenGL 渲染器创建一个临时
`QGuiApplication`、一个 `QWindow` 和一个 `QOpenGLContext`，随后销毁。在 Qt 6 中，销毁这个探测 `QWindow` 时
会调用 `QAccessible::isActive()`（Qt 5.15 的 `QWindow` 析构函数里没有这个判断），这次调用会经 xcb 或 wayland 的
QPA 插件实例化无障碍实现（at-spi 桥），从而建立 session bus 连接—— **建立连接时进程里已经存在 `qApp`，而它恰好
是这个临时的 `QGuiApplication`**，这一点决定了后面由谁来"恢复投递"。

Qt 的 D-Bus 连接是 **进程级**资源。当连接在主线程上建立、且进程里已存在 `qApp` 时
（`bool suspendedDelivery = QThread::isMainThread() && qApp;`），`QDBusConnectionManager::doConnectToStandardBus()`
会执行 `if (c && suspendedDelivery) d->setDispatchEnabled(false);`，即 **先把投递挂起**，再用
`QMetaObject::invokeMethod` 把"稍后（事件循环开始后）把 `dispatchEnabled` 置回 `true`"排队投递给 **那个 `qApp`**。
需要说明的是：`dispatchEnabled == false` 本身是正常应用也会经历的 **临时初始态**（`exec()` 运行后即被置回 `true`
并清空积压消息）；真正致命的是下面两步。

那个临时的 `QGuiApplication` 在任何事件循环运行之前就被销毁，于是这个排队调用随接收者一起被撤销，
`setDispatchEnabled(true)` 永远不会执行；而连接对象被全局的 `QDBusConnectionManager` 持有
（`connectionHash` 加上裸指针 `defaultBuses[]`），应用析构时 QtDBus 不做任何清理。之后真实应用的
`QDBusConnection::sessionBus()` 命中 `if (defaultBuses[type]) return defaultBuses[type];` 直接早退，拿到的仍是
这条 `dispatchEnabled == false` 的连接， **并且不会再重新安排一次恢复**。于是进程再也不回应任何入站的 D-Bus
方法调用，全局菜单就为空了。

### 1.2 缓解措施

根据原因分析可知，只需阻止无障碍实现建立 session bus 连接即可缓解该问题。我询问 DeepSeek 该怎样阻止，
其分析 Qt 源码后说明：把 `AT_SPI_BUS_ADDRESS` 设为非空值，`QAtSpiDBusConnection` 的构造函数会走 early-return，
直接改用该变量指定的无障碍总线，从而根本不会调用 `QDBusConnection::sessionBus()`。
副作用是：该地址无效时，该进程的无障碍功能会失效。

经实际测试，`AT_SPI_BUS_ADDRESS=1 krita` 实例的全局菜单正常，与上述分析一致。

## 二、调用链

此部分由 DeepSeek V4.1 Flash 生成。

### 2.1 Krita 侧：真实应用之前先构造了一个完整而短命的应用对象

`krita/main.cc:442` → `KisOpenGL::selectSurfaceConfig()`（`libs/ui/opengl/kis_opengl.cpp:910`，定义处）→
`KisOpenGLModeProber::probeFormat(config, adjustGlobalState = true)`
（`libs/ui/opengl/KisOpenGLModeProber.cpp`）。这段代码本身 **已经主动避免让探测应用接触 D-Bus**：

```cpp
// libs/ui/opengl/KisOpenGLModeProber.cpp（节选，行号为实际值）
QScopedPointer<EnvironmentSetter> portalSetter;
portalSetter.reset(new EnvironmentSetter(QLatin1String("QT_NO_XDG_DESKTOP_PORTAL"), QLatin1String("1")));   // 183 行
...
if (runningInKDE && !isInAppimage)
    QGuiApplication::setDesktopSettingsAware(false);                  // 192 行
...
application.reset(new QGuiApplication(argc, &argv));                  // 195 行
...
QWindow surface;                                                      // 203 行
surface.create();                                                     // 206 行
QOpenGLContext context; ...                                           // GL 探测
// 函数返回：QWindow 先析构，随后 QGuiApplication 析构
```

* `QT_NO_XDG_DESKTOP_PORTAL=1` 阻止基于 portal 的设置监听（`QDesktopUnixServices`）在探测期间建立连接；
* `setDesktopSettingsAware(false)` 阻止在探测应用里创建 KDE 平台主题（及其 `KIconLoader` / `KConfigWatcher`
  的 D-Bus 使用）。

另外，该文件 **从不调用 `show()`**（已 grep 确认），探测窗口只 `create()`、从不显示、也从不获得焦点，
因此不会产生显示/焦点类无障碍事件。

`krita/main.cc:377` 另有那个临时 `QCoreApplication`，实测无害（只有 core 应用时不会建立 D-Bus 连接）。

### 2.2 Qt 6 新增：`QWindow` 析构里的无障碍查询（Qt 5 没有）

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

Qt 5.15 的析构函数里 **完全没有无障碍代码**：

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

Qt 6 这段代码的意图是合理的：在宣告"某窗口的可访问对象消失了"之前，先判断无障碍是否启用
（`QAccessible::isActive()` 的文档正是 **建议**把它当作"避免昂贵通知"的廉价守卫使用）。
问题在于：`QAccessible::isActive()` 自身确实很廉价——它只是转发给 `QPlatformAccessibility::isActive()`
（即 `return m_active;`）——但 **QPA 集成把"取得平台无障碍对象"实现成了懒加载**：
`QXcbIntegration::accessibility()` / `QWaylandIntegration::accessibility()` 会在第一次被查询时
`new QSpiAccessibleBridge()`。于是这个"廉价守卫"带上了"实例化 at-spi 桥并建立 D-Bus 连接"的副作用，
与该文档建议的用法/意图不符。

在探测应用里，其它任何地方都不会碰无障碍：Qt 6 中一个裸 `QWindow` 与无障碍相关的调用点只有 4 处
（`~QWindow` 的 ObjectDestroyed、`QWindow::setParent` 的 ParentChanged、`QWindow::event()` 的 FocusIn/FocusOut），
而 `QWindow` **根本不发送创建/显示类事件**。探测窗口从未 show、从未获得焦点，因此 **它的析构就是探测应用整个
生命里第一次、也是唯一一次索要无障碍实现**。

### 2.3 这次查询经 QPA 插件实例化 at-spi 桥并建立 D-Bus 连接

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

gdb 实证—— **该调用栈采集自最小复现器**（其探测结构与 Krita 的 `probeFormat()` 相同），并非直接 attach 真实 Krita；
真实 Krita 在 Qt 6 下的跨进程探针结果与之完全一致。方法：在 `QDBusConnection::sessionBus()` 上打断点，取探测期间的第一次命中：

```
#0 QDBusConnection::sessionBus()                      libQt6DBus
#1 ??                                                 libQt6Gui   （即 QAtSpiDBusConnection 构造）
#2 QSpiAccessibleBridge::QSpiAccessibleBridge()       libQt6Gui
#3 QXcbIntegration::accessibility() const              libQt6XcbQpa
#4 QAccessible::isActive()                             libQt6Gui
#5 QWindow::~QWindow()                                 libQt6Gui   ← 触发点
#6 探测迭代（结构同 KisOpenGLModeProber）
#7 main()
```

### 2.4 Qt 的 D-Bus 连接是进程级的，而"恢复投递"绑定在被销毁的那个 qApp 上

**（a）连接属于进程，不属于任何应用对象。** 它由全局单例持有
（`Q_GLOBAL_STATIC(QDBusConnectionManager, _q_manager)`），存放在 `connectionHash` 中，并被裸指针
`defaultBuses[type]` 缓存，靠 `QDBusConnectionPrivate::ref`（初值 1）维持生命。探测应用的 `QGuiApplication`
析构只会执行 `QGuiApplicationPrivate::cleanup()`（`delete platform_theme; delete platform_integration;`，
`qguiapplication.cpp:1886-1891`），也就是只销毁这条连接的 **使用者**；QtDBus 没有任何"随应用销毁而清理"的逻辑，
全局 manager 直到进程退出才销毁（而它的析构也只是停掉自己的线程）。

**（b）"稍后恢复投递"这一步绑定在建立连接时存在的那个 `qApp` 上。**

```cpp
// src/dbus/qdbusconnectionmanager.cpp
bool suspendedDelivery = QThread::isMainThread() && qApp;     // true → qApp 就是那个临时探测应用
...
if (suspendedDelivery && result && result->connection)
    result->enableDispatchDelayed(qApp);                      // 排队调用投给探测应用
```

```cpp
// src/dbus/qdbusintegrator.cpp
void QDBusConnectionPrivate::enableDispatchDelayed(QObject *context)
{
    ref.ref();
    QMetaObject::invokeMethod(context, [this]() {              // 接收者 == 那个 qApp
        QMetaObject::invokeMethod(this, &QDBusConnectionPrivate::setDispatchEnabled, Qt::QueuedConnection, true);
        ...
    }, Qt::QueuedConnection);
}
```

探测应用在任何事件循环运行之前就被销毁 → 这个排队调用随接收者一起消失 → `setDispatchEnabled(true)` 永不执行。

**（c）缓存下来的连接不会被重新关联到新的 qApp。** 之后每一次 `QDBusConnection::sessionBus()`
（包括真实 `KisApplication` 的调用）都会命中这个早退：

```cpp
if (defaultBuses[type])
    return defaultBuses[type];      // 直接返回缓存指针；新的 qApp 不会重新安排"延迟恢复"
```

`defaultBuses[]` 只在 manager 构造函数里被初始化为 `nullptr`，此后永不重置；
`QDBusConnection::disconnectFromBus("qt_default_session_bus")` 也清不掉它（`removeConnection()` 只删
`connectionHash` 里的条目）。于是这条连接在进程余下的整个生命周期里始终 `dispatchEnabled == false`，
它与那个已销毁 `qApp` 的关联也 **永远处于失效状态**。

**（d）可观察结果：**

```cpp
// src/dbus/qdbusintegrator.cpp — QDBusConnectionPrivate::handleMessage()
if (!dispatchEnabled && !isLocal) {
    qDBusDebug() << this << "delivery is suspended";
    pendingMessages << amsg;
    return amsg.type() == QDBusMessage::MethodCallMessage;   // 记为"已处理"，但永不回包
}
```

所有入站方法调用都被入队且永不回应（连错误也不返回）。只有连接层的
`org.freedesktop.DBus.Peer.Ping` 仍有应答（它不经过这个闸门），这也是它容易被误诊的原因。

**（e）为什么表现为"面板空 **且** 窗口内没有菜单栏"：** 外呼与阻塞式调用正常，所以 `RegisterWindow` 成功、
Plasma 也记住了 `(:service, /MenuBar/N)`；但 applet 对该对象的 `GetLayout` / `AboutToShow` 永远收不到应答而超时。
同时 Qt 认为存在原生全局菜单栏，于是把窗口内菜单栏隐藏。`KDE_NO_GLOBAL_MENU=1` 会让 `createPlatformMenuBar()`
返回 `nullptr`，Qt 就把菜单栏画回窗口内。

## 三、为什么回报给 KDE 而不是 Qt

此部分由 DeepSeek V4.1 Flash 生成。

* 在 Krita 真实运行之前构造那个应用对象的过程中，已有代码排除了大部分可能产生 D-Bus 连接的功能。
  由此猜测 Krita 此前可能已经考虑过 Qt D-Bus 连接管理器的行为。
* 基于这一点，本报告先提交给 KDE：它记录的是 Krita 侧可观察到的症状、可复现步骤以及一个可在 Krita 侧落地的
  缓解办法（见 1.2）；修复 Krita 侧也足以消除该症状。
* 不过从机制上看，这更像是 Qt 的缺陷：
    * 一是连接管理器将连接缓存在 `defaultBuses[]`，
      而它与 `qApp` 的绑定（`enableDispatchDelayed(qApp)`）不会随应用对象更替而更新，也不会在交出缓存连接时重新安排；
    * 二是 `QAccessible::isActive()` 会因 QPA 集成把 `accessibility()` 实现为懒加载而实例化
      at-spi 桥、建立 D-Bus 连接，这与该函数被建议的使用方式（廉价守卫）不符。
* 我并未确认 Qt 那边是否已有相关问题报告。如果有熟悉 Qt 的开发者能够确认这确实是 Qt 的 bug，希望您能帮忙向 Qt 报告。
  Qt 侧更详细的技术描述见随附的 `QTBUG-REPORT.md`。

## 四、相关报告

* KDE bug 483170 — appmenu (global menu) doesn't work with krita on plasma 6
* KDE bug 515889 — [qt6] Application Menu is unavailable on Krita 6
* KDE bug 518583 — Global menu broken on KDE Plasma after commit 2c920c28 (Remove QT_QPA_PLATFORMTHEME)
* Krita 提交 `8d6a2a5d12` — "Do not load the platform theme when created a test QApplication"（BUG 408015）

---

> **Translator’s note:** The Chinese original is authoritative. This English version was translated by DeepSeek V4.1
> Flash. In the Chinese original, the sentence “此部分由 DeepSeek V4.1 Flash 生成。” literally means “This section was
> generated by DeepSeek V4.1 Flash.” It indicates that the corresponding part of the Chinese original was generated by AI;
> it does not refer to this English translation.

# Root Cause Analysis: Plasma Global Menu Blank under Krita 6 (Qt 6)

The cause was identified through analysis by DeepSeek V4.1 Flash, and a minimal reproduction is provided for
verification.

**Environment:** Arch Linux, KDE Plasma 6 (Wayland session, Krita running under XWayland, `QT_QPA_PLATFORM=xcb`), Qt
**6.11.2**, KF6 6.30.0, `plasma-integration` 6.7.5; Krita master `351297cdce` (6.1.0-prealpha) and 6.0.4; compared
against Qt 5.15.19  
**Report date:** 2026-10-03

## I. Overview

This section gives a brief analysis and mitigations.

### 1.1 Root Cause

Before Krita’s real application object exists, `KisOpenGLModeProber` creates a temporary `QGuiApplication`, a `QWindow`,
and a `QOpenGLContext` in order to probe the OpenGL renderer, and then destroys them. In Qt 6, destroying this probe
`QWindow` calls `QAccessible::isActive()` (Qt 5.15’s `QWindow` destructor did not have this check). This call
instantiates the accessibility implementation (the at-spi bridge) through the xcb or wayland QPA plugin, thereby
establishing a session bus connection— **when the connection is established, `qApp` already exists in the process, and
it happens to be this temporary `QGuiApplication`**, which determines who will later “resume delivery.”

Qt’s D-Bus connections are **process-level** resources. When a connection is established on the main thread and `qApp`
already exists in the process (`bool suspendedDelivery = QThread::isMainThread() && qApp;`),
`QDBusConnectionManager::doConnectToStandardBus()` executes `if (c && suspendedDelivery) d->setDispatchEnabled(false);`,
i.e. **it first suspends delivery**, then uses `QMetaObject::invokeMethod` to queue “later (after the event loop starts)
set `dispatchEnabled` back to `true`” for delivery to **that `qApp`**. It should be noted that
`dispatchEnabled == false` itself is a **temporary initial state** that normal applications also go through (after
`exec()` runs it is set back to `true` and backlog messages are cleared); what is truly fatal are the following two
steps.

That temporary `QGuiApplication` is destroyed before any event loop runs, so this queued call is cancelled together with
its receiver, and `setDispatchEnabled(true)` is never executed; the connection object is held globally by
`QDBusConnectionManager` (`connectionHash` plus the raw pointer `defaultBuses[]`), and QtDBus does not perform any
cleanup when the application is destroyed. Afterwards, the real application’s `QDBusConnection::sessionBus()` hits
`if (defaultBuses[type]) return defaultBuses[type];` and returns early, still getting this connection with
`dispatchEnabled == false`, **and it will not schedule another resumption**. Therefore, the process never again responds
to any inbound D-Bus method calls, and the global menu is empty.

### 1.2 Mitigation

From the root cause analysis, it can be seen that simply preventing the accessibility implementation from establishing a
session bus connection is enough to mitigate the problem. I asked DeepSeek how to prevent it. After analyzing the Qt
source code, it explained: set `AT_SPI_BUS_ADDRESS` to a non-empty value; then `QAtSpiDBusConnection`’s constructor will
take an early return and directly use the accessibility bus specified by that variable, thus never calling
`QDBusConnection::sessionBus()`. The side effect is: if that address is invalid, accessibility in that process will stop
working.

Actual testing confirmed: the global menu of an instance launched as `AT_SPI_BUS_ADDRESS=1 krita` works normally,
consistent with the above analysis.

## II. Call Chain

> **Translator’s note:** In the Chinese original, this section is preceded by “此部分由 DeepSeek V4.1 Flash 生成。”,
> meaning this section of the Chinese original was generated by AI. Rendered literally: “This section was generated by
> DeepSeek V4.1 Flash.”

### 2.1 Krita Side: A Complete but Short-Lived Application Object Is Constructed Before the Real Application

`krita/main.cc:442` → `KisOpenGL::selectSurfaceConfig()` (`libs/ui/opengl/kis_opengl.cpp:910`, definition) →
`KisOpenGLModeProber::probeFormat(config, adjustGlobalState = true)` (`libs/ui/opengl/KisOpenGLModeProber.cpp`). This
code itself **already actively avoids letting the probe application touch D-Bus**:

```cpp
// libs/ui/opengl/KisOpenGLModeProber.cpp (excerpt; line numbers are actual values)
QScopedPointer<EnvironmentSetter> portalSetter;
portalSetter.reset(new EnvironmentSetter(QLatin1String("QT_NO_XDG_DESKTOP_PORTAL"), QLatin1String("1")));   // line 183
...
if (runningInKDE && !isInAppimage)
    QGuiApplication::setDesktopSettingsAware(false);                  // line 192
...
application.reset(new QGuiApplication(argc, &argv));                  // line 195
...
QWindow surface;                                                      // line 203
surface.create();                                                     // line 206
QOpenGLContext context; ...                                           // GL probing
// Function returns: QWindow is destructed first, then QGuiApplication is destructed
```

* `QT_NO_XDG_DESKTOP_PORTAL=1` prevents portal-based settings monitoring (`QDesktopUnixServices`) from establishing a
  connection during probing;
* `setDesktopSettingsAware(false)` prevents creation of the KDE platform theme (and its `KIconLoader` / `KConfigWatcher`
  D-Bus usage) in the probe application.

In addition, this file **never calls `show()`** (confirmed by grep); the probe window is only `create()`d, never shown,
and never receives focus, so it does not produce display/focus accessibility events.

`krita/main.cc:377` also has that temporary `QCoreApplication`, which was measured to be harmless (with only a core
application, no D-Bus connection is established).

### 2.2 New in Qt 6: Accessibility Query in the `QWindow` Destructor (Not in Qt 5)

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

Qt 5.15’s destructor has **no accessibility code at all**:

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

The intent of this Qt 6 code is reasonable: before announcing that “some window’s accessible object has disappeared,”
first check whether accessibility is enabled (the documentation of `QAccessible::isActive()` indeed **recommends** using
it as a cheap guard to “avoid expensive notifications”). The problem is: `QAccessible::isActive()` itself is indeed
cheap—it just forwards to `QPlatformAccessibility::isActive()` (i.e. `return m_active;`)—but **the QPA integration
implements “get the platform accessibility object” as lazy initialization**: `QXcbIntegration::accessibility()` /
`QWaylandIntegration::accessibility()` will, on first query, `new QSpiAccessibleBridge()`. Thus this “cheap guard”
carries the side effect of “instantiating the at-spi bridge and establishing a D-Bus connection,” which does not match
the documented recommended use/intent.

In the probe application, nothing else touches accessibility: in Qt 6, a bare `QWindow` has only 4 accessibility-related
call sites (`~QWindow`’s ObjectDestroyed, `QWindow::setParent`’s ParentChanged, `QWindow::event()`’s FocusIn/FocusOut),
and `QWindow` **does not send creation/show events at all**. The probe window is never shown and never focused, so **its
destruction is the first and only time in the probe application’s entire life that it asks for the accessibility
implementation**.

### 2.3 This Query Instantiates the at-spi Bridge via the QPA Plugin and Establishes a D-Bus Connection

```cpp
// src/gui/accessible/qaccessible.cpp
static QPlatformAccessibility *platformAccessibility()
{
    QPlatformIntegration *pfIntegration = QGuiApplicationPrivate::platformIntegration();
    return pfIntegration ? pfIntegration->accessibility() : nullptr;      // virtual function → QPA plugin
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

`QSpiAccessibleBridge`’s constructor creates `QAtSpiDBusConnection`, which opens the bus
(`src/gui/accessible/linux/dbusconnection.cpp`):

```cpp
QAtSpiDBusConnection::QAtSpiDBusConnection(QObject *parent) : ... {
    QByteArray addressEnv = qgetenv("AT_SPI_BUS_ADDRESS");
    if (!addressEnv.isEmpty()) {                 // if non-empty, session bus is not touched at all
        m_enabled = true;
        connectA11yBus(QString::fromLocal8Bit(addressEnv));
        return;
    }
    QDBusConnection c = QDBusConnection::sessionBus();   // ← the process-level session bus is established here
    ...
```

gdb evidence— **this call stack was collected from a minimal reproducer** (whose probe structure is the same as Krita’s
`probeFormat()`), not by directly attaching to real Krita; real Krita’s cross-process probe result under Qt 6 is exactly
the same. Method: set a breakpoint on `QDBusConnection::sessionBus()` and take the first hit during probing:

```
#0 QDBusConnection::sessionBus()                      libQt6DBus
#1 ??                                                 libQt6Gui   (i.e. QAtSpiDBusConnection constructor)
#2 QSpiAccessibleBridge::QSpiAccessibleBridge()       libQt6Gui
#3 QXcbIntegration::accessibility() const              libQt6XcbQpa
#4 QAccessible::isActive()                             libQt6Gui
#5 QWindow::~QWindow()                                 libQt6Gui   ← trigger point
#6 probe iteration (structure same as KisOpenGLModeProber)
#7 main()
```

### 2.4 Qt’s D-Bus Connection Is Process-Level, and “Resume Delivery” Is Bound to the Destroyed qApp

**(a) The connection belongs to the process, not to any application object.** It is held by a global singleton
(`Q_GLOBAL_STATIC(QDBusConnectionManager, _q_manager)`), stored in `connectionHash`, and cached by the raw pointer
`defaultBuses[type]`, kept alive by `QDBusConnectionPrivate::ref` (initial value 1). Destruction of the probe
application’s `QGuiApplication` only executes `QGuiApplicationPrivate::cleanup()`
(`delete platform_theme; delete platform_integration;`, `qguiapplication.cpp:1886-1891`), i.e. it only destroys the
connection’s **user**; QtDBus has no logic to “clean up when the application is destroyed,” and the global manager is
destroyed only when the process exits (and its destructor merely stops its own thread).

**(b) The “resume delivery later” step is bound to the qApp that existed when the connection was established.**

```cpp
// src/dbus/qdbusconnectionmanager.cpp
bool suspendedDelivery = QThread::isMainThread() && qApp;     // true → qApp is that temporary probe application
...
if (suspendedDelivery && result && result->connection)
    result->enableDispatchDelayed(qApp);                      // queue the call for the probe application
```

```cpp
// src/dbus/qdbusintegrator.cpp
void QDBusConnectionPrivate::enableDispatchDelayed(QObject *context)
{
    ref.ref();
    QMetaObject::invokeMethod(context, [this]() {              // receiver == that qApp
        QMetaObject::invokeMethod(this, &QDBusConnectionPrivate::setDispatchEnabled, Qt::QueuedConnection, true);
        ...
    }, Qt::QueuedConnection);
}
```

The probe application is destroyed before any event loop runs → this queued call disappears with its receiver →
`setDispatchEnabled(true)` is never executed.

**(c) The cached connection is not re-associated with the new qApp.** Every later `QDBusConnection::sessionBus()`
(including the real `KisApplication`’s call) hits this early return:

```cpp
if (defaultBuses[type])
    return defaultBuses[type];      // directly returns the cached pointer; the new qApp will not schedule “delayed resume” again
```

`defaultBuses[]` is initialized to `nullptr` only in the manager constructor and is never reset afterwards;
`QDBusConnection::disconnectFromBus("qt_default_session_bus")` cannot clear it either (`removeConnection()` only deletes
the entry in `connectionHash`). Therefore, for the remainder of the process’s entire lifetime, this connection remains
`dispatchEnabled == false`, and its association with the destroyed qApp **remains invalid forever**.

**(d) Observable result:**

```cpp
// src/dbus/qdbusintegrator.cpp — QDBusConnectionPrivate::handleMessage()
if (!dispatchEnabled && !isLocal) {
    qDBusDebug() << this << "delivery is suspended";
    pendingMessages << amsg;
    return amsg.type() == QDBusMessage::MethodCallMessage;   // recorded as “handled”, but never replies
}
```

All inbound method calls are queued and never answered (not even an error is returned). Only the connection-level
`org.freedesktop.DBus.Peer.Ping` still responds (it does not pass through this gate), which is why it is easy to
misdiagnose.

**(e) Why it manifests as “panel empty **and** no menu bar inside the window”:** Outgoing and blocking calls work
normally, so `RegisterWindow` succeeds and Plasma remembers `(:service, /MenuBar/N)`; but the applet’s `GetLayout` /
`AboutToShow` on that object never receives a reply and times out. At the same time, Qt believes a native global menu
bar exists, so it hides the in-window menu bar. `KDE_NO_GLOBAL_MENU=1` makes `createPlatformMenuBar()` return `nullptr`,
and Qt draws the menu bar back inside the window.

## III. Why This Was Reported to KDE Rather Than Qt

> **Translator’s note:** In the Chinese original, this section is preceded by “此部分由 DeepSeek V4.1 Flash 生成。”,
> meaning this section of the Chinese original was generated by AI. Rendered literally: “This section was generated by
> DeepSeek V4.1 Flash.”

* During the process of constructing that application object before Krita really runs, existing code already excludes
  most functionality that could produce a D-Bus connection. From this, one may guess that Krita had probably already
  considered the behavior of Qt’s D-Bus connection manager.
* Based on this, this report was first submitted to KDE: it records the symptoms observable on the Krita side,
  reproduction steps, and a mitigation that can be implemented on the Krita side (see 1.2); fixing the Krita side is
  also sufficient to eliminate the symptom.
* However, from the mechanism perspective, this looks more like a Qt defect:
    * First, the connection manager caches connections in `defaultBuses[]`, but its binding with `qApp`
      (`enableDispatchDelayed(qApp)`) is not updated when the application object is replaced, nor is it rescheduled when
      the cached connection is handed out.
    * Second, `QAccessible::isActive()` instantiates the at-spi bridge and establishes a D-Bus connection because the
      QPA integration implements `accessibility()` lazily; this does not match how the function is recommended to be
      used (as a cheap guard).
* I have not confirmed whether there is already a related Qt report. If a developer familiar with Qt can confirm that
  this is indeed a Qt bug, I hope you can help report it to Qt. A more detailed technical description for the Qt side is
  in the accompanying `QTBUG-REPORT.md`.

## IV. Related Reports

* KDE bug 483170 — appmenu (global menu) doesn't work with krita on plasma 6
* KDE bug 515889 — [qt6] Application Menu is unavailable on Krita 6
* KDE bug 518583 — Global menu broken on KDE Plasma after commit 2c920c28 (Remove QT_QPA_PLATFORMTHEME)
* Krita commit `8d6a2a5d12` — "Do not load the platform theme when created a test QApplication" (BUG 408015)
