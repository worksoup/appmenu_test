# 原因分析：Krita 6（Qt 6）下 Plasma 全局菜单空白

> English version: [`BUGREPORT.en.md`](BUGREPORT.en.md)

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

## 四、相关报告

* KDE bug 483170 — appmenu (global menu) doesn't work with krita on plasma 6
* KDE bug 515889 — [qt6] Application Menu is unavailable on Krita 6
* KDE bug 518583 — Global menu broken on KDE Plasma after commit 2c920c28 (Remove QT_QPA_PLATFORMTHEME)
* Krita 提交 `8d6a2a5d12` — "Do not load the platform theme when created a test QApplication"（BUG 408015）

---

> English version: [`BUGREPORT.en.md`](BUGREPORT.en.md)
