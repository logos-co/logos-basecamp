// srcdeps: WorkspaceArea.cpp
//
// Unit tests for WorkspaceArea.
// Verifies dock lifecycle, widget ownership, tab bar state, and
// wheel/close behavior.
//
//   nix build .#unit-tests -L
#include "WorkspaceArea.h"

#include <QtTest/QtTest>
#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QPointer>
#include <QSignalSpy>
#include <QIcon>
#include <QPixmap>
#include <QQuickWidget>
#include <QTabBar>
#include <QToolButton>
#include <QWheelEvent>

namespace {

// Let QTimer::singleShot(0, ...) callbacks run — WorkspaceArea uses them
// heavily to style tab bars after adds/removes.
void processDeferred()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

QWidget* makePluginWidget(const QString& label)
{
    auto* w = new QLabel(label);
    w->setObjectName("pluginWidget_" + label);
    w->setAttribute(Qt::WA_DontShowOnScreen);
    return w;
}

QTabBar* tabBarOf(WorkspaceArea& ws)
{
    const auto bars = ws.findChildren<QTabBar*>();
    return bars.isEmpty() ? nullptr : bars.first();
}

int dockCount(WorkspaceArea& ws)
{
    // Flush deleteLater() so a just-removed dock doesn't inflate the count.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    int count = 0;
    for (auto* dock : ws.findChildren<QDockWidget*>()) {
        if (dock->objectName() == QLatin1String("__phantom_tab_placeholder__"))
            continue;
        ++count;
    }
    return count;
}

int tabIndexFor(QTabBar* bar, const QString& text)
{
    for (int i = 0; i < bar->count(); ++i)
        if (bar->tabText(i) == text) return i;
    return -1;
}

} // namespace

class WorkspaceAreaTest : public QObject {
    Q_OBJECT

private slots:
    // --- Basic lifecycle ---------------------------------------------------

    void addSingleDockCreatesDockChild()
    {
        WorkspaceArea ws;
        QCOMPARE(dockCount(ws), 0);

        QWidget* w = makePluginWidget("A");
        ws.addPluginDock(w, "A");

        QCOMPARE(dockCount(ws), 1);
        QVERIFY(ws.dockFor("A") != nullptr);
        QCOMPARE(ws.nameForWidget(w), QString("A"));
    }

    void addSecondDockTabifiesWithFirst()
    {
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QCOMPARE(dockCount(ws), 2);
        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QCOMPARE(bar->count(), 2);
        QVERIFY(tabIndexFor(bar, "A") >= 0);
        QVERIFY(tabIndexFor(bar, "B") >= 0);
    }

    void addWithDuplicateNameActivatesExisting()
    {
        WorkspaceArea ws;
        QWidget* first = makePluginWidget("A");
        QWidget* second = makePluginWidget("A");

        ws.addPluginDock(first, "A");
        ws.addPluginDock(second, "A");   // duplicate → activate existing
        processDeferred();

        QCOMPARE(dockCount(ws), 1);
        QVERIFY(ws.dockFor("A") != nullptr);
        // First is registered under "A"; second was never adopted.
        // We assert via nameForWidget (semantic API) rather than
        // dock->widget() (which is now a DockCard wrapper).
        QCOMPARE(ws.nameForWidget(first), QString("A"));
        QCOMPARE(ws.nameForWidget(second), QString());

        // `second` is orphaned (never adopted by any dock). Delete it
        // ourselves — no reparenting happened so there's no race.
        delete second;
    }

    // --- Removing the first (anchor) dock (bug #8) -----------------------

    void removeFirstDockThenAddNewAnchorsCorrectly()
    {
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        ws.addPluginDock(makePluginWidget("C"), "C");
        processDeferred();

        ws.removePluginDock("A");   // the original anchor
        processDeferred();

        QCOMPARE(dockCount(ws), 2);
        QVERIFY(ws.dockFor("A") == nullptr);
        QVERIFY(ws.dockFor("B") != nullptr);
        QVERIFY(ws.dockFor("C") != nullptr);

        // New dock must tabify with the remaining group, not spawn detached.
        ws.addPluginDock(makePluginWidget("D"), "D");
        processDeferred();

        QCOMPARE(dockCount(ws), 3);
        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QCOMPARE(bar->count(), 3);
    }

    void removeLastDockAllowsFreshFirstDock()
    {
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.removePluginDock("A");
        processDeferred();

        // m_firstDock is now nullptr — a new add must re-anchor.
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QCOMPARE(dockCount(ws), 1);
        QVERIFY(ws.dockFor("B") != nullptr);
    }

    // --- pluginClosed signal round-trip -----------------------------------

    void tabCloseRequestEmitsPluginClosedWithModuleName()
    {
        // Tab × click emits pluginClosed(moduleName) and lets the consumer
        // drive teardown via the unload path (so cascade gating runs and
        // ViewModuleHost is stopped so the plugin's destructor fires).
        // WorkspaceArea does NOT remove the dock itself on × click —
        // removePluginDock is called later via pluginWindowRemoveRequested.
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QSignalSpy spy(&ws, &WorkspaceArea::pluginClosed);
        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);

        const int bIdx = tabIndexFor(bar, "B");
        QVERIFY(bIdx >= 0);

        emit bar->tabCloseRequested(bIdx);
        processDeferred();

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), QString("B"));
        // Dock is still present — the unload path (not wired in this test)
        // is responsible for removal.
        QVERIFY(ws.dockFor("B") != nullptr);
        QVERIFY(ws.dockFor("A") != nullptr);
    }

    void tabCloseEmitsModuleNameNotDisplayLabel()
    {
        // Two docks so a tab bar exists (single docks don't tabify).
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("accounts_ui"), "accounts_ui", "Accounts");
        ws.addPluginDock(makePluginWidget("wallet_ui"),   "wallet_ui",   "Wallet");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QCOMPARE(ws.dockFor("accounts_ui")->objectName(),
                 QString("accounts_ui"));                    // stable id
        QCOMPARE(ws.dockFor("accounts_ui")->windowTitle(),
                 QString("Accounts"));                       // display label

        const int accountsIdx = tabIndexFor(bar, "Accounts");
        QVERIFY(accountsIdx >= 0);

        QSignalSpy spy(&ws, &WorkspaceArea::pluginClosed);
        emit bar->tabCloseRequested(accountsIdx);
        processDeferred();

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), QString("accounts_ui"));
    }

    // --- Stress (row #16 — repeated open/close does not crash) ------------

    void repeatedAddRemoveDoesNotCrash()
    {
        WorkspaceArea ws;
        for (int i = 0; i < 30; ++i) {
            const QString name = QString("dock-%1").arg(i);
            ws.addPluginDock(makePluginWidget(name), name);
            processDeferred();
            ws.removePluginDock(name);
            processDeferred();
        }
        QCOMPARE(dockCount(ws), 0);
    }

    // --- Section-switch hide/show (bug #4, partial) -----------------------

    void hideShowCyclesCleanly()
    {
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.show();
        processDeferred();
        QVERIFY(ws.isVisible());

        ws.hide();
        processDeferred();
        QVERIFY(!ws.isVisible());

        ws.show();
        processDeferred();
        QVERIFY(ws.isVisible());
    }

    // --- Wheel scroll (bug #5) --------------------------------------------

    void wheelScrollXSwitchesTab()
    {
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        const int before = bar->currentIndex();

        QWheelEvent ev(QPointF(bar->rect().center()), QPointF(bar->mapToGlobal(bar->rect().center())),
                       QPoint(120, 0), QPoint(120, 0),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(bar, &ev);
        processDeferred();

        QVERIFY2(bar->currentIndex() != before,
            "Horizontal wheel over the tab bar did not switch tabs");
    }

    void wheelScrollYAlsoSwitchesTabViaQtDefaultHandler()
    {
        // Confirms overall behavior. The custom handler in eventFilter
        // reads only .x() and consumes only X wheel events, but Y wheel
        // events fall through to QTabBar's default handler which
        // switches the tab. Net effect: wheel-switch works on both axes.
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        const int before = bar->currentIndex();

        QWheelEvent ev(QPointF(bar->rect().center()), QPointF(bar->mapToGlobal(bar->rect().center())),
                       QPoint(0, 120), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(bar, &ev);
        processDeferred();

        QVERIFY2(bar->currentIndex() != before,
            "Vertical wheel did not switch tabs via QTabBar default handler");
    }

    // --- Close-button regen on add (bug #7 / #11) -------------------------

    void closeButtonsScaleWithTabsWithoutLeaking()
    {
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);

        auto activeCloseButtons = [bar]() {
            int n = 0;
            for (int i = 0; i < bar->count(); ++i)
                if (bar->tabButton(i, QTabBar::LeftSide) != nullptr) n++;
            return n;
        };

        QCOMPARE(activeCloseButtons(), 2);

        ws.addPluginDock(makePluginWidget("C"), "C");
        processDeferred();
        QCOMPARE(activeCloseButtons(), 3);

        // Loose bound: no runaway QToolButton siblings on the tab bar.
        const int childButtons = bar->findChildren<QToolButton*>().size();
        QVERIFY2(childButtons <= bar->count() * 3,
            qPrintable(QString("QToolButton children ballooned to %1 for %2 tabs")
                       .arg(childButtons).arg(bar->count())));
    }

    // --- Scale — many docks open simultaneously ---------------------------
    //
    // The existing repeatedAddRemoveDoesNotCrash test is a *lifecycle*
    // stress (add-then-remove serially, never >1 concurrent). These are
    // *concurrent* stress: high dock counts alive at once, exercising:
    //   * QMap<QString,QDockWidget*> lookup at scale
    //   * styleAllTabBars iterating every tab bar × every button
    //     (would show any O(n²) restyle cost)
    //   * QTabBar overflow/elision behavior
    //   * close-button count growth from repeated installTabBarCloseButtons
    //     calls (bug #11 guard at scale, not just at N=3)

    void manyDocksOpenSimultaneously_15()
    {
        WorkspaceArea ws;
        for (int i = 0; i < 15; ++i) {
            const QString name = QString("app%1").arg(i);
            ws.addPluginDock(makePluginWidget(name), name);
        }
        processDeferred();

        QCOMPARE(dockCount(ws), 15);
        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QCOMPARE(bar->count(), 15);

        // Every dock reachable via dockFor.
        for (int i = 0; i < 15; ++i)
            QVERIFY(ws.dockFor(QString("app%1").arg(i)) != nullptr);

        // Close-button total stays bounded — install re-runs each add,
        // deleteLater eventually flushes the old ones. Allow a generous
        // headroom (3× tab count) but catch runaway growth.
        const int buttons = bar->findChildren<QToolButton*>().size();
        QVERIFY2(buttons <= bar->count() * 3,
            qPrintable(QString("Close buttons grew to %1 for %2 tabs")
                       .arg(buttons).arg(bar->count())));
    }

    void manyDocksRemoveInterleavedOrder_stress()
    {
        WorkspaceArea ws;
        const int total = 20;
        for (int i = 0; i < total; ++i) {
            const QString name = QString("app%1").arg(i);
            ws.addPluginDock(makePluginWidget(name), name);
        }
        processDeferred();
        QCOMPARE(dockCount(ws), total);

        // Remove every other one — mid-life churn that touches the
        // first-dock anchor logic (index 0) plus middle removes.
        for (int i = 0; i < total; i += 2)
            ws.removePluginDock(QString("app%1").arg(i));
        processDeferred();

        QCOMPARE(dockCount(ws), total / 2);
        for (int i = 0; i < total; ++i) {
            const bool shouldRemain = (i % 2 == 1);
            const QString name = QString("app%1").arg(i);
            QCOMPARE(ws.dockFor(name) != nullptr, shouldRemain);
        }

        // Add 5 more on top of the surviving 10 — checks that add path
        // still anchors correctly after heavy prior churn.
        for (int i = total; i < total + 5; ++i) {
            const QString name = QString("app%1").arg(i);
            ws.addPluginDock(makePluginWidget(name), name);
        }
        processDeferred();

        QCOMPARE(dockCount(ws), total / 2 + 5);
        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QCOMPARE(bar->count(), total / 2 + 5);
    }

    // --- Framing (transparent bg, zero margins) --------------------------
    //
    // Regression guard for the "fit whole screen, match window color"
    // fix. If someone later reintroduces autoFillBackground(true) or
    // reinstates content margins, the workspace stops merging visually
    // into the surrounding shell.

    void workspacePaintsOpaqueMatchingShellColor()
    {
        // Must be opaque + non-translucent — on macOS,
        // WA_TranslucentBackground makes tab-drag gestures fall through
        // to the window-frame drag handler and hijack window-move.
        // Opaque + shell-matching color gives the same visual without
        // the drag conflict.
        WorkspaceArea ws;
        QVERIFY2(ws.autoFillBackground(),
            "WorkspaceArea must paint its own opaque background — "
            "translucent leaks drag events to the OS window frame on macOS.");
        QVERIFY2(!ws.testAttribute(Qt::WA_TranslucentBackground),
            "WorkspaceArea must NOT set WA_TranslucentBackground — "
            "on macOS this makes drag-tab conflict with window-move.");
        // The color must match MainContainer's shell (#171717) so
        // there's no visible seam between shell and workspace.
        const QColor expected("#171717");
        QCOMPARE(ws.palette().color(QPalette::Window), expected);
    }

    void workspaceHasZeroContentMargins()
    {
        WorkspaceArea ws;
        QCOMPARE(ws.contentsMargins(), QMargins(0, 0, 0, 0));
    }

    // --- Tab icons propagate from dock windowIcon -------------------------
    //
    // Qt does NOT auto-copy dock->windowIcon() to the tab bar when
    // docks are tabified (unlike QMdiArea, which did). WorkspaceArea's
    // styleAllTabBars restores it manually. If this regresses, tabs go
    // icon-less and users can't distinguish apps at a glance.

    void tabIconsPropagateFromDockWindowIcon()
    {
        WorkspaceArea ws;

        QPixmap redPm(16, 16);  redPm.fill(Qt::red);
        QPixmap bluePm(16, 16); bluePm.fill(Qt::blue);

        QWidget* a = makePluginWidget("A");
        a->setWindowIcon(QIcon(redPm));
        QWidget* b = makePluginWidget("B");
        b->setWindowIcon(QIcon(bluePm));

        ws.addPluginDock(a, "A");
        ws.addPluginDock(b, "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QCOMPARE(bar->count(), 2);

        for (int i = 0; i < bar->count(); ++i) {
            QVERIFY2(!bar->tabIcon(i).isNull(),
                qPrintable(QString("Tab %1 (%2) has no icon — dock->windowIcon() "
                                   "not being propagated to the tab bar")
                           .arg(i).arg(bar->tabText(i))));
        }
    }

    void tabIconsAbsentWhenWidgetHasNoWindowIcon()
    {
        // Widgets with no windowIcon must not cause the restore loop to
        // set a spurious icon, and must not crash.
        WorkspaceArea ws;
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        for (int i = 0; i < bar->count(); ++i)
            QVERIFY(bar->tabIcon(i).isNull());
    }

    // --- Icon refresh on live plugin widget (basecamp#137) ----------------
    //
    // After a .lgx reinstall, UIPluginManager reloads the plugin widget's
    // windowIcon in place

    void iconChangeOnPluginWidget_propagatesToDock()
    {
        WorkspaceArea ws;

        QPixmap redPm(16, 16);  redPm.fill(Qt::red);
        QPixmap bluePm(16, 16); bluePm.fill(Qt::blue);

        QWidget* w = makePluginWidget("A");
        w->setWindowIcon(QIcon(redPm));
        ws.addPluginDock(w, "A");
        processDeferred();

        QDockWidget* dock = ws.dockFor("A");
        QVERIFY(dock != nullptr);
        const auto initialKey = dock->windowIcon().cacheKey();
        QVERIFY(!dock->windowIcon().isNull());

        // Simulate the reinstall: UIPluginManager calls setWindowIcon
        // with a fresh QIcon on the widget. This fires WindowIconChange.
        w->setWindowIcon(QIcon(bluePm));
        processDeferred();

        QVERIFY2(dock->windowIcon().cacheKey() != initialKey,
                 "Dock windowIcon must update when the plugin widget's "
                 "windowIcon changes — WindowIconChange event filter is "
                 "the propagation mechanism (basecamp#137).");
    }

    void iconChangeOnPluginWidget_propagatesToTabBar()
    {
        WorkspaceArea ws;

        QPixmap redPm(16, 16);   redPm.fill(Qt::red);
        QPixmap bluePm(16, 16);  bluePm.fill(Qt::blue);
        QPixmap greenPm(16, 16); greenPm.fill(Qt::green);

        // Two docks so a tab bar is guaranteed (single dock has no tab bar).
        QWidget* a = makePluginWidget("A");
        a->setWindowIcon(QIcon(redPm));
        QWidget* b = makePluginWidget("B");
        b->setWindowIcon(QIcon(bluePm));

        ws.addPluginDock(a, "A");
        ws.addPluginDock(b, "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        const int iA = tabIndexFor(bar, "A");
        QVERIFY2(iA >= 0, "Tab for plugin A must exist");
        const auto initialKey = bar->tabIcon(iA).cacheKey();
        QVERIFY(!bar->tabIcon(iA).isNull());

        a->setWindowIcon(QIcon(greenPm));
        processDeferred();

        QVERIFY2(bar->tabIcon(iA).cacheKey() != initialKey,
                 "Tab-bar icon for A must update when plugin widget A's "
                 "windowIcon changes (basecamp#137).");
    }

    // Regression: setWindowIcon on plugin B must NOT touch dock A's icon.
    // Guards against the event filter mis-matching widget → dock via
    // (say) always taking the first dock, or forgetting the widget-equality
    // check inside the eventFilter branch.
    void iconChangeIsScopedToCorrectDock()
    {
        WorkspaceArea ws;

        QPixmap redPm(16, 16);  redPm.fill(Qt::red);
        QPixmap bluePm(16, 16); bluePm.fill(Qt::blue);
        QPixmap greenPm(16, 16); greenPm.fill(Qt::green);

        QWidget* a = makePluginWidget("A");
        a->setWindowIcon(QIcon(redPm));
        QWidget* b = makePluginWidget("B");
        b->setWindowIcon(QIcon(bluePm));

        ws.addPluginDock(a, "A");
        ws.addPluginDock(b, "B");
        processDeferred();

        const auto initialAKey = ws.dockFor("A")->windowIcon().cacheKey();

        // Change only B's icon.
        b->setWindowIcon(QIcon(greenPm));
        processDeferred();

        QCOMPARE(ws.dockFor("A")->windowIcon().cacheKey(), initialAKey);
    }

    // --- Welcome-page central widget --------------------------------------
    //
    // WorkspaceArea shows a QML WelcomePage as its central widget when
    // constructed with a backend and no docks are open. All existing
    // tests use the no-backend constructor to keep the 0×0 placeholder
    // (avoids spinning up a QML runtime). These tests exercise the
    // backend-provided constructor's visibility contract only —
    // rendering + backend binding is a doctest-layer concern.

    void welcomePageAbsentWithoutBackend()
    {
        WorkspaceArea ws;
        QVERIFY(ws.welcomePageWidget() == nullptr);
    }

    void welcomePageVisibleWhenNoDocks()
    {
        // Bare QObject as backend — WorkspaceArea only forwards it as a
        // QML context property; QML tolerates missing props gracefully.
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        QVERIFY(ws.welcomePageWidget() != nullptr);
        QVERIFY2(ws.welcomePageWidget()->isVisible() || ws.welcomePageWidget()->isVisibleTo(&ws),
            "Welcome page must be visible when no docks are open");
    }

    void welcomePageHiddenWhenDockAdded()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        processDeferred();
        QVERIFY2(!ws.welcomePageWidget()->isVisibleTo(&ws),
            "Welcome page must hide as soon as a dock exists");
    }

    void welcomePageReappearsWhenLastDockRemoved()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        processDeferred();
        ws.removePluginDock("A");
        processDeferred();
        QVERIFY2(ws.welcomePageWidget()->isVisibleTo(&ws),
            "Welcome page must reappear once all docks are closed");
    }

    // --- Widget ownership — plugin dies with dock (Qt cascade) ------------
    //
    // When removePluginDock is called (typically by the unload path via
    // pluginWindowRemoveRequested), the dock is deleteLater'd and the
    // plugin widget dies with it via Qt's parent-child cascade — that's
    // what triggers the plugin's own destructor. Consumers must NOT hold
    // references to the plugin widget after removePluginDock returns.
    void removePluginDockDestroysPluginWidget()
    {
        WorkspaceArea ws;
        QWidget* w = makePluginWidget("A");
        QPointer<QWidget> track(w);

        ws.addPluginDock(w, "A");
        ws.removePluginDock("A");

        processDeferred();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        QVERIFY2(track.isNull(),
            "Plugin widget should be destroyed with the dock via Qt "
            "parent-child cascade — consumers rely on this to trigger "
            "the plugin's own destructor.");
    }
};

QTEST_MAIN(WorkspaceAreaTest)
#include "workspace_area_test.moc"
