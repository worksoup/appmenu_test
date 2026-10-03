# appmenu_test — minimal visual reproducer for the empty Plasma global menu on Krita 6 / Qt 6

> **Translator’s note:** the Chinese original — [`README.md`](README.md) — is authoritative; this file is a
> translation of it.

A minimal windowed reproducer: it creates a throw-away `QGuiApplication` **before the real `QApplication`** (exactly
what Krita’s OpenGL prober `KisOpenGLModeProber::probeFormat()` does) and then opens an ordinary window.

- **Qt 6**: the Plasma panel’s Application Menu (global menu) stays **empty**, and the window shows **no** menu bar.
- **Qt 5**: the global menu shows File / Edit / Help as usual.

The program itself prints **nothing** (except for `--help` and `--verbose`); just watch the window and the panel.
It can be built against Qt 5 and Qt 6.

## Symptoms

| | Panel global menu | In-window menu bar |
|---|---|---|
| `plain` (control) | File / Edit / Help | none (normal: the menu was given to the global menu) |
| `probe` / `krita` (Qt 6) | **empty** | **none** ← BUG |
| `probe` / `krita` (Qt 5) | File / Edit / Help | none |

`KDE_NO_GLOBAL_MENU=1` moves the menu bar back into the window (the same in every mode); this is the known temporary
workaround for Krita.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure      # smoke test: --help of both variants
```

A single build tree produces both variants, according to what is available:

- `build/appmenu_test-qt6`
- `build/appmenu_test-qt5`

If you only want one of them:

```sh
cmake -S . -B build -DAPPMENU_TEST_QT5=OFF      # Qt 6 only
cmake -S . -B build -DAPPMENU_TEST_QT6=OFF      # Qt 5 only
```

## Running

```sh
./build/appmenu_test-qt6 plain      # control: the panel global menu should show File/Edit/Help
./build/appmenu_test-qt6 probe      # reproduces the BUG
./build/appmenu_test-qt6 krita      # closest to Krita: throw-away QCoreApplication + 3 probe applications
./build/appmenu_test-qt6 core       # only the throw-away QCoreApplication (harmless)
./build/appmenu_test-qt5 probe      # the same path on Qt 5: fine
```

> If `SESSION_MANAGER` is present in the shell while `~/.config` is not writable, Qt opens a modal
> “Configuration file ... not writable” dialog that blocks the program. Avoid it with
> `env -u SESSION_MANAGER XDG_CONFIG_HOME=/tmp/appmenu-cfg ./build/appmenu_test-qt6 probe`.

### Options

| Option | Effect |
|---|---|
| `--probes N` | number of throw-away `QGuiApplication`s (`probe`: 1, `krita`: 3) |
| `--bare-probe` | the probe application only does `new QGuiApplication`, without a QWindow (**this does not reproduce**) |
| `--block-a11y` | sets `AT_SPI_BUS_ADDRESS` non-empty in the probe application, so the a11y bridge never touches the session bus — **the effective workaround** |
| `--reset-a11y` | `disconnectFromBus("a11y")` after the probes, dropping the dead a11y connection the probe left behind |
| `--no-portal-guard` | do not set `QT_NO_XDG_DESKTOP_PORTAL=1` in the probe application (**Qt 5 breaks too**; it must still be kept when using `--block-a11y`) |
| `--no-workaround` | do not call `QGuiApplication::setDesktopSettingsAware(false)` (no effect on Qt 6) |
| `--block-dbus` | point `DBUS_SESSION_BUS_ADDRESS` at a dead address during the probe (**measured useless and harmful**, see below) |
| `--reset-bus` | `disconnectFromBus("qt_default_session_bus")` after the probes (**measured useless**, see below) |
| `--verbose` | print phase markers, `winId`, `isNativeMenuBar` and the `/MenuBar/1` registration state to stderr (for scripted diagnostics) |
| `--wayland` | force `QT_QPA_PLATFORM=wayland` (default: Krita’s xcb / XWayland) |
| `--hidden` | do not show the window (for scripted diagnostics) |
| `--help` | usage |

To see the fixed state (the panel global menu then shows File/Edit/Help):

```sh
./build/appmenu_test-qt6 probe --block-a11y --reset-a11y
```

## Necessary conditions for reproducing (measured)

| What the probe application does | Qt 6.11.2 | Qt 5.15.19 |
|---|---|---|
| only `new QGuiApplication` (`--bare-probe`) | 0 / 0 fine | 0 / 0 fine |
| **+ create a `QWindow` and destroy it later** (default, same as Krita’s prober) | **124 / 124 reproduces** | 0 / 0 fine |
| + as above, but without the portal guard | 124 / 124 | **124 / 124 (Qt 5 breaks too)** |

(The values are the exit status of `Introspect` / `AboutToShow` issued from another process against the exported
object; `124` means it timed out without an answer.)

## Can it be fixed with “a switch during the probe”? (measured)

Yes — and **without blocking the whole session bus**: it is enough to keep the accessibility (at-spi) bridge away
from the session bus while the probe application is alive. The mechanism is in Qt 6’s
`src/gui/accessible/linux/dbusconnection.cpp`:

```cpp
QAtSpiDBusConnection::QAtSpiDBusConnection(QObject *parent) : ... {
    QByteArray addressEnv = qgetenv("AT_SPI_BUS_ADDRESS");
    if (!addressEnv.isEmpty()) {            // non-empty -> bypasses the session bus entirely
        m_enabled = true;
        connectA11yBus(QString::fromLocal8Bit(addressEnv));
        return;
    }
    QDBusConnection c = QDBusConnection::sessionBus();   // ← what creates the connection during the probe
    ...
```

The probe application therefore no longer creates a session-bus connection, and the process’ first connection is
created by the **real application** → `enableDispatchDelayed(qApp)` lands on the real `qApp` → delivery resumes when
`exec()` runs → the global menu works.

| Configuration (`probe` mode, measured on Qt 6.11.2) | `isNativeMenuBar` | `/MenuBar/1` registered | cross-process `Introspect`/`AboutToShow` | Verdict |
|---|---|---|---|---|
| baseline | 1 | 1 | **124 / 124** | reproduces the bug |
| `--block-a11y` | 1 | 1 | **0 / 0** | ✅ fixed |
| `--block-a11y --reset-a11y` | 1 | 1 | **0 / 0** | ✅ fixed (and no dead a11y connection left) |
| `--block-a11y --no-portal-guard` | 1 | 1 | 124 / 124 | the portal guard is still required |
| `--no-portal-guard` | 1 | 1 | 124 / 124 | the portal creates a connection |
| `--bare-probe` | 1 | 1 | 0 / 0 | no window in the probe → never reproduced in the first place |
| `--block-dbus` | **0** | **0** | no object to probe | ❌ the whole session bus dies (not even a global menu is left) |
| `--reset-bus` | 1 | 1 | 124 / 124 | ❌ no effect |

Both failing approaches are explained by the Qt sources:

* `--block-dbus`: in `QDBusConnectionManager::doConnectToStandardBus()`, `setConnection(name, d)` runs
  unconditionally **before** `d->setConnection(c /* == nullptr */, error)`, so even a *failed* connection is cached;
  `busConnection()` then returns that dead connection forever ⇒ the whole process loses the session bus (not just the
  global menu: KConfig/KIO/notifications stop working as well).
* `--reset-bus`: `QDBusConnection::disconnectFromBus("qt_default_session_bus")` →
  `QDBusConnectionManager::removeConnection()` only removes the entry from `connectionHash` and **does not touch
  `defaultBuses[]`** (in the whole project only the manager’s constructor sets it to `nullptr`), while
  `busConnection()` returns `defaultBuses[type]` directly whenever it is non-null ⇒ the real application still gets
  that `delivery suspended` connection. By contrast `--reset-a11y` works, because `a11y` is only a named connection
  in `connectionHash` and is not in `defaultBuses[]`, so it really can be dropped.

Key points:

* `AT_SPI_BUS_ADDRESS` must be set **only while the probe application is alive**. Setting it for the whole process
  also “fixes” the bug, but points the entire process’ accessibility at a fake address (measured: a process-wide
  `AT_SPI_BUS_ADDRESS=unix:path=...` gives 0/0 as well). This program uses the RAII `EnvironmentSetter` to restore it.
* No `Qt::AA_*` attribute or public API can “switch accessibility off”: measured process-wide `QT_ACCESSIBILITY=0`
  and `NO_AT_BRIDGE=1` both have no effect (the bridge still creates the session bus);
  `QT_LINUX_ACCESSIBILITY_ALWAYS_ON` forces it *on*.
* This is still a workaround: it depends on the implementation detail that the at-spi bridge early-returns whenever
  `AT_SPI_BUS_ADDRESS` is non-empty. The root fix is still not to create a `QGuiApplication` before the real
  application (probe in a child process / probe inside the real application).

## Root cause summary

1. When a `qApp` already exists, Qt 6’s `QDBusConnectionManager::busConnection()` creates the session-bus connection
   in **“suspended delivery”** mode (`suspendedDelivery`) and calls `result->enableDispatchDelayed(qApp)`; the latter
   is a **queued invocation posted to `qApp`**, which only runs `setDispatchEnabled(true)` inside that application’s
   event loop.
2. The probe application is destroyed before entering any event loop → that queued invocation disappears together
   with its receiver → the connection stays at `dispatchEnabled == false` forever. The connection object lives in the
   global `QDBusConnectionManager` and is cached (`defaultBuses[]`); when the real application obtains it,
   `if (defaultBuses[type]) return defaultBuses[type];` returns it directly and no new “resume” is ever scheduled.
3. As a result, `handleMessage()`’s `if (!dispatchEnabled && !isLocal)` throws every external method call into
   `pendingMessages` and **never replies** → Plasma’s applet can only time out → the global menu stays empty. Only the
   connection-level built-in `Peer.Ping` still answers (so testing Ping alone is misleading).
4. Qt 5 implements the same “resume delivery” with `QTimer::singleShot(0, enabler, ...)`, but **Qt 5 hangs just as
   permanently whenever the connection was created while the probe application was alive** (last row of the table
   above). The reason Qt 5 is fine in Krita is that Krita’s prober sets `QT_NO_XDG_DESKTOP_PORTAL=1`, blocking the
   only trigger at the time (the XDG portal); under Qt 6, **destroying that `QWindow`** in the probe application still
   opens the session bus: `QWindow::~QWindow()` calls `QAccessible::isActive()`, which makes the QPA plugin lazily
   instantiate the at-spi bridge (`src/gui/accessible/linux/dbusconnection.cpp`) → `QDBusConnection::sessionBus()`.
   The probe window is only `create()`d and never `show()`n, so this is the only accessibility query in that
   application’s entire life; neither of the two existing guards stops it — only `AT_SPI_BUS_ADDRESS` (see the
   previous section) does.

The corresponding Krita code: `libs/ui/opengl/KisOpenGLModeProber.cpp`, `probeFormat()`
(`new QGuiApplication` plus `QWindow surface; surface.create();`), reached from
`krita/main.cc` → `KisOpenGL::selectSurfaceConfig()`.

## Related records

- KDE Bug 483170 “appmenu (global menu) doesn’t work with krita on plasma 6”
- KDE Bug 515889 “[qt6] Application Menu is unavailable on Krita 6”
- Krita’s historical fix `8d6a2a5d12` (“Do not load the platform theme when created a test QApplication”,
  BUG 408015): its `setDesktopSettingsAware(false)` is no longer enough on Qt 6 to prevent the session-bus
  connection from being created
- Qt sources: `src/dbus/qdbusconnectionmanager.cpp` (`busConnection` / `connectToBus`),
  `src/dbus/qdbusintegrator.cpp` (`enableDispatchDelayed` / `handleMessage`)

Full root-cause analysis (QtDBus internals, measured matrices, KDE-side mitigation and suggested fixes):
[`BUGREPORT.en.md`](BUGREPORT.en.md) (English) · [`BUGREPORT.md`](BUGREPORT.md) (Chinese, authoritative).

---

> Chinese original (authoritative): [`README.md`](README.md)
