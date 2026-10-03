# QTBUG — QDBusConnection delivery stays permanently suspended when the session bus is created inside a short-lived second QGuiApplication

**Tracker:** https://bugreports.qt.io — project **QTBUG**, component **DBus** (related: *GUI: Accessibility*)
**Reported:** 2026-10-03 (root cause analysis + minimal reproducer by the **DeepSeek Harness coding agent**)
**Qt:** 6.11.2 (Arch Linux `qt6-base 6.11.2-3`), also checked against the 6.10.2 sources (identical code paths)
**Platform:** Linux, KDE Plasma 6 session (Wayland; reproduced with both the `xcb`/XWayland and the `wayland` platform plugin)
**Also affects:** real applications — Krita 6 (KDE bugs [483170](https://bugs.kde.org/show_bug.cgi?id=483170), [515889](https://bugs.kde.org/show_bug.cgi?id=515889)) loses its Plasma global menu because of this.

---

## Summary

If the process' **session-bus connection is created while a short-lived second `QGuiApplication` exists** (e.g. an
OpenGL-renderer probing application that is destroyed before the real `QApplication` is created), then the real
application **never replies to incoming method calls on its exported D-Bus objects**, even though

* the connection is alive (registration and outgoing/blocking calls work), and
* the exported objects exist and are registered (`/MenuBar/1` in the example, `com.canonical.dbusmenu` interface).

Only the connection-level `org.freedesktop.DBus.Peer.Ping` is answered. `QDBUS_DEBUG=1` prints
`delivery is suspended` for every incoming call.

## Root cause

1. `QDBusConnectionManager::busConnection()` (`src/dbus/qdbusconnectionmanager.cpp`) creates the standard bus connection in
   *suspended delivery* mode whenever a `QCoreApplication` already exists, and caches it in a raw pointer array:

   ```cpp
   // we'll start in suspended delivery mode if we're in the main thread
   // (the event loop will resume delivery) and QCoreApplication already exists
   bool suspendedDelivery = QThread::isMainThread() && qApp;
   ...
   if (defaultBuses[type])                       // cached -> never re-armed
       return defaultBuses[type];
   return defaultBuses[type] = connectToBus(type, name, suspendedDelivery);
   ```

2. `connectToBus()` schedules the "resume delivery" step as a **queued invocation whose receiver is the current
   application object**:

   ```cpp
   if (suspendedDelivery && result && result->connection)
       result->enableDispatchDelayed(qApp);      // qApp == the throw-away application
   ```

   ```cpp
   void QDBusConnectionPrivate::enableDispatchDelayed(QObject *context)   // src/dbus/qdbusintegrator.cpp
   {
       ref.ref();
       QMetaObject::invokeMethod(context, [this]() {                  // receiver == qApp
           QMetaObject::invokeMethod(this, &QDBusConnectionPrivate::setDispatchEnabled,
                                     Qt::QueuedConnection, true);
           if (!ref.deref())
               deleteLater();
       }, Qt::QueuedConnection);
   }
   ```

3. The throw-away application is destroyed **before any event loop runs**, so that queued invocation is dropped together
   with its receiver and `setDispatchEnabled(true)` is never executed. The connection object lives in the global
   `QDBusConnectionManager` and is pinned in `defaultBuses[]`, so the real application receives the very same connection
   and nothing ever re-arms the enable: `dispatchEnabled` stays `false` forever.

4. `QDBusConnectionPrivate::handleMessage()` therefore takes this branch for every externally received call:

   ```cpp
   if (!dispatchEnabled && !isLocal) {
       qDBusDebug() << this << "delivery is suspended";
       pendingMessages << amsg;
       return amsg.type() == QDBusMessage::MethodCallMessage;   // reported as handled, but no reply is ever sent
   }
   ```

   The peer (e.g. the Plasma global-menu applet) simply times out; because no error reply is sent either, the failure looks
   like a hung application.

### What opens the session bus inside the throw-away application

It is Qt's own at-spi accessibility bridge, `QAtSpiDBusConnection`
(`src/gui/accessible/linux/dbusconnection.cpp`), which is instantiated when the throw-away application creates its first
`QWindow`:

```cpp
QAtSpiDBusConnection::QAtSpiDBusConnection(QObject *parent) : ... {
    QByteArray addressEnv = qgetenv("AT_SPI_BUS_ADDRESS");
    if (!addressEnv.isEmpty()) {                 // non-empty -> never touches the session bus
        m_enabled = true;
        connectA11yBus(QString::fromLocal8Bit(addressEnv));
        return;
    }
    QDBusConnection c = QDBusConnection::sessionBus();   // <-- creates the process-wide session bus here
    ...
```

Note that the usual mitigations for "throw-away application touches global state" do **not** prevent this:
`QGuiApplication::setDesktopSettingsAware(false)` (no KDE platform theme) and `QT_NO_XDG_DESKTOP_PORTAL=1` (no portal
settings watcher) were both set in the reproducer, and the connection was still created during the throw-away app.

## Steps to reproduce

Minimal example (`repro.cpp`, linked against Qt6::Widgets Qt6::DBus):

```cpp
#include <QApplication>
#include <QMainWindow>
#include <QMenuBar>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QWindow>

int main(int argc, char **argv) {
    // 1) throw-away QGuiApplication, like an OpenGL renderer prober
    {
        int argc1 = 1;
        QByteArray name("probe");
        char *argv1[] = { name.data(), nullptr };
        QGuiApplication probe(argc1, argv1);

        QWindow surface;
        surface.setSurfaceType(QSurface::OpenGLSurface);
        surface.create();                 // needed: this is what makes the session bus open here

        QOpenGLContext context;
        context.create();
    }

    // 2) the real application
    QApplication app(argc, argv);
    QMainWindow w;
    w.menuBar()->addMenu("File")->addAction("Open");
    w.show();
    return app.exec();
}
```

Run it, then from a **second process** (the window must stay open; no user interaction is needed). The easiest way to get
the application's bus name is to print `QDBusConnection::sessionBus().baseService()` in the reproducer; alternatively match
it by pid:

```sh
name=$(busctl --user list --no-pager | awk -v p=$(pgrep -x repro) '$2==p {print $1; exit}')

# The object exists and answers the connection-level Ping ...
gdbus call --session --dest "$name" --object-path / --method org.freedesktop.DBus.Peer.Ping
# -> ()

# ... but every real method call is never answered (timeout, not an error):
gdbus call --session --dest "$name" --object-path /MenuBar/1 \
      --method com.canonical.dbusmenu.AboutToShow 0
# -> no reply (gdbus hangs until its timeout, exit status 124)
gdbus call --session --dest "$name" --object-path /MenuBar/1 \
      --method com.canonical.dbusmenu.GetLayout 0 1 "@as []"
# -> no reply
gdbus call --session --dest "$name" --object-path / \
      --method org.freedesktop.DBus.Introspectable.Introspect
# -> no reply
```

With `QDBUS_DEBUG=1` the application itself prints, for each of those calls:

```
QDBusConnectionPrivate(0x...) got message (signal): QDBusMessage(type=MethodCall, ... member="AboutToShow" ...)
QDBusConnectionPrivate(0x...) delivery is suspended
```

If the throw-away `QGuiApplication` is removed (or if it does not create a `QWindow`), everything works normally.

### Measured matrix (Qt 6.11.2; values are the exit status of a cross-process `Introspect` / `AboutToShow`; 124 = timeout)

| configuration | `QMenuBar::isNativeMenuBar()` | `/MenuBar/1` registered | cross-process call |
|---|---|---|---|
| plain application | true | true | 0 / 0 |
| throw-away `QGuiApplication` **without** a `QWindow` | true | true | 0 / 0 |
| **throw-away `QGuiApplication` + `QWindow`** | true | true | **124 / 124** |
| + `AT_SPI_BUS_ADDRESS=1` while the throw-away app is alive | true | true | **0 / 0** |
| + poison `DBUS_SESSION_BUS_ADDRESS` while the throw-away app is alive | false | false | object not registered at all |

## Expected behaviour

Creating and destroying a second `QGuiApplication` is certainly not supported (and the application doing it is at fault),
but the resulting state should not be "this process never answers any incoming D-Bus method call, silently". At minimum:

* the "resume delivery" step should not depend on the *first* application object surviving (`enableDispatchDelayed(qApp)`),
  and/or
* when `busConnection()` hands out a cached connection that is still suspended, it should re-arm the delayed enable.

## Secondary issue (same file)

`QDBusConnectionManager::doConnectToStandardBus()` caches the connection object **before** it knows whether the connection
succeeded:

```cpp
d = new QDBusConnectionPrivate;
... c = q_dbus_bus_get_private(DBUS_BUS_SESSION, error);
setConnection(name, d);              // cached unconditionally
d->setConnection(c, error);          // c may be nullptr
```

so a failed connection attempt (e.g. a deliberately blocked `DBUS_SESSION_BUS_ADDRESS` during a probe) leaves a dead
connection pinned in `defaultBuses[]`, and
`QDBusConnection::disconnectFromBus("qt_default_session_bus")` cannot get rid of it (it only removes the entry from
`connectionHash`; `removeConnection()` never touches `defaultBuses[]`, and `busConnection()` returns that stale pointer
whenever it is non-null). Suggest either not caching failed connections, or clearing `defaultBuses[type]` in
`disconnectFromBus()`.

## Workaround (application side, verified)

Keep the at-spi bridge off the session bus for the lifetime of the throw-away application by setting
`AT_SPI_BUS_ADDRESS` to any non-empty value (the bridge then takes its early-return path and never calls
`QDBusConnection::sessionBus()`); the process' first session-bus connection is then created by the real application, whose
event loop does run, so the delayed enable happens normally:

```cpp
qputenv("AT_SPI_BUS_ADDRESS", "1");        // restored after the probe app is gone
{ QGuiApplication probe(...); ... }
qunsetenv("AT_SPI_BUS_ADDRESS");
// optional: drop the probe app's accessibility connection so the real app does not inherit a dead one
QDBusConnection::disconnectFromBus("a11y");
```

Scope it to the throw-away application: setting it for the whole process also hides the problem but silently disables
accessibility for that process.

## Comparison with Qt 5

Qt 5.15 has the same "start suspended, resume later" idea, but the resume step is a `QTimer::singleShot(0, enabler, ...)`
on a self-owned `QDBusConnectionDispatchEnabler` rather than a queued invocation on `qApp`. The outcome is the same when
the connection really is created inside the throw-away application (Qt 5.15.19 breaks identically in that case — measured),
but applications whose probe app does not open the session bus are unaffected there, which is why the same pattern has been
working for years on Qt 5 (e.g. Krita's OpenGL prober).

## Reproducer attachment

A complete windowed reproducer for Qt 5 and Qt 6 (CMake, ~250 lines, no other dependencies) is available and can be
attached on request:

```
MODE = plain | core | probe | krita      # probe/krita create the throw-away QGuiApplication (+ QWindow)
options: --bare-probe --block-a11y --reset-a11y --block-dbus --reset-bus --verbose --hidden
```

## Related reports

* KDE bug 483170 — appmenu (global menu) doesn't work with krita on plasma 6
* KDE bug 515889 — [qt6] Application Menu is unavailable on Krita 6
* Krita commit `8d6a2a5d12` — "Do not load the platform theme when created a test QApplication" (BUG 408015)

---

## 中文摘要（供参考，正式提交以上面英文为准）

**现象**：进程里若在真正的 `QApplication` 之前存在过一个**短命的一次性 `QGuiApplication`**（例如 OpenGL 渲染器探测程序，
它还会创建 `QWindow`），那么真实应用**永远不会回应外部对其导出 D-Bus 对象的方法调用**——连接是活的（注册、阻塞式外呼都正常），
对象也确实注册了，但除连接层的 `Peer.Ping` 外一律超时；`QDBUS_DEBUG=1` 打印 `delivery is suspended`。

**根因**：`QDBusConnectionManager::busConnection()`（`src/dbus/qdbusconnectionmanager.cpp`）只要 `qApp` 已存在，就以
"先挂起投递"模式建立标准总线连接并缓存裸指针；`connectToBus()` 把"恢复投递"做成**投给 `qApp` 的排队调用**
（`QDBusConnectionPrivate::enableDispatchDelayed(qApp)`）。一次性应用在事件循环运行前就被销毁 → 该排队调用随接收者消失，
`setDispatchEnabled(true)` 永不执行；连接被 `defaultBuses[]` 全局钉住复用，再无人重新安排恢复 →
`handleMessage()` 对每个外部调用走 `if (!dispatchEnabled && !isLocal) { pendingMessages << amsg; return ...; }`
（入队、不回包、也不报错）。而在 Qt6 下，是**无障碍（at-spi）桥**在一次性应用创建 `QWindow` 时调用了
`QDBusConnection::sessionBus()`（`src/gui/accessible/linux/dbusconnection.cpp`），且
`setDesktopSettingsAware(false)`、`QT_NO_XDG_DESKTOP_PORTAL=1` 都挡不住它。

**期望**：延迟恢复不应依赖"建立连接时的那个应用对象存活"；或在 `busConnection()` 交出仍是 suspended 的缓存连接时重新安排恢复。
另外建议不要把**失败**的连接也永久缓存（`doConnectToStandardBus()` 里先 `setConnection(name, d)` 再
`d->setConnection(c, error)`），否则 `disconnectFromBus()` 也无法清除 `defaultBuses[]` 里的死指针。

**缓解**：在一次性应用存活期间设 `AT_SPI_BUS_ADDRESS` 为任意非空值（如 `1`），使 a11y 桥 early-return、不去建立 session bus；
探测结束后可选 `QDBusConnection::disconnectFromBus("a11y")`。应只在该作用域内设置（全程设置会静默禁用该进程的无障碍）。
