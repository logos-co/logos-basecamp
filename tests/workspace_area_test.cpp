// srcdeps: WorkspaceArea.cpp ShellDevQml.cpp
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
#include <QQuickItem>
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

    // The welcome page is a permanent tab, not the central widget, so opening
    // an app no longer destroys or detaches it — it stops being the CURRENT
    // tab while staying available to click back to. Asserting on its dock
    // rather than on visibility, because "not current" and "gone" look the
    // same through isVisibleTo().
    void welcomePageSurvivesAsATabWhenDockAdded()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        processDeferred();

        QVERIFY2(ws.welcomePageWidget() != nullptr,
            "Welcome page must outlive opening an app");
        auto* dock = ws.findChild<QDockWidget*>(QStringLiteral("__welcome_tab__"));
        QVERIFY2(dock != nullptr, "Welcome tab's dock must still exist");
        QVERIFY2(!dock->features().testFlag(QDockWidget::DockWidgetClosable),
            "Welcome tab must not be closeable");
    }

    // Its tab carries no text — that emptiness is what keeps it out of the
    // close-button, close-request and active-app paths, so it is worth pinning
    // down rather than leaving as an implementation detail.
    void welcomeTabHasNoTitleAndNoCloseButton()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        processDeferred();

        auto* dock = ws.findChild<QDockWidget*>(QStringLiteral("__welcome_tab__"));
        QVERIFY(dock != nullptr);
        QVERIFY2(dock->windowTitle().isEmpty(),
            "Welcome tab must stay icon-only");

        for (QTabBar* bar : ws.findChildren<QTabBar*>()) {
            for (int i = 0; i < bar->count(); ++i) {
                if (!bar->tabText(i).isEmpty()) continue;
                QVERIFY2(bar->tabButton(i, QTabBar::LeftSide) == nullptr,
                    "Welcome tab must not carry a close button");
            }
        }
    }

    // ── Welcome tab regressions ──────────────────────────────────────────
    // Each of these pins something that broke at least once while the welcome
    // page was being turned into a permanent tab.

    // Adding the welcome tab must not cost the app tabs their close button.
    // installTabBarCloseButtons() skips "furniture" tabs, and the definition of
    // furniture changed twice — first empty text, then empty text OR the spacer
    // sentinel — so it is worth asserting the app tabs still get one.
    void appTabsKeepTheirCloseButtons()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);

        int appTabs = 0;
        for (int i = 0; i < bar->count(); ++i) {
            const QString text = bar->tabText(i);
            if (text.isEmpty() || text.startsWith("__")) continue;   // furniture
            ++appTabs;
            QVERIFY2(bar->tabButton(i, QTabBar::LeftSide) != nullptr,
                     qPrintable(QString("app tab '%1' lost its close button")
                                    .arg(text)));
        }
        QCOMPARE(appTabs, 2);
    }

    // The close buttons must survive repeated layout passes. They were being
    // created correctly and then destroyed again: QTabBar::moveTab drops a
    // tab's buttons, and the tab-pinning code ran moveTab on every relayout.
    //
    // Mirrors the real startup sequence deliberately — resize, show, THEN add
    // the dock, then pump raw events. An earlier version of this test used the
    // shared processDeferred() helper and passed even with the bug present,
    // because the destroy-then-recreate race settles differently when the
    // event loop is drained with a timeout.
    void closeButtonsSurviveRepeatedRelayouts()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.resize(1200, 700);
        ws.show();
        for (int i = 0; i < 8; ++i) QCoreApplication::processEvents();

        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        for (int i = 0; i < 8; ++i) QCoreApplication::processEvents();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);

        int appTabs = 0;
        for (int i = 0; i < bar->count(); ++i) {
            const QString text = bar->tabText(i);
            if (text.isEmpty() || text.startsWith("__")) continue;
            ++appTabs;
            QVERIFY2(bar->tabButton(i, QTabBar::LeftSide) != nullptr,
                     qPrintable(QString("tab '%1' lost its close button")
                                    .arg(text)));
        }
        QCOMPARE(appTabs, 1);
    }

    // The welcome tab's icon must sit centred in its plate. Qt LEFT-aligns an
    // icon inside a content box wider than the icon, so any slack in the
    // stylesheet's min/max-width lands entirely on the right and the icon hugs
    // the left edge. Asserted by rendering: the tab's own geometry says nothing
    // about where the icon was actually painted.
    void welcomeTabIconIsCentred()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.resize(1200, 700);
        ws.show();
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QVERIFY(bar->tabText(0).isEmpty());

        // The real icon lives in the main_ui plugin's qrc, which a unit test
        // does not link — so paint a recognisable stand-in. styleAllTabBars()
        // only assigns when its own lookup succeeds, so this survives.
        QPixmap pm(bar->iconSize());
        pm.fill(QColor(255, 0, 255));
        bar->setTabIcon(0, QIcon(pm));
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        const QImage shot = bar->grab().toImage();
        int iconL = INT_MAX, iconR = -1, row = -1;
        for (int y = 0; y < shot.height() && iconR < 0; ++y) {
            for (int x = 0; x < shot.width(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                if (c.red() > 200 && c.blue() > 200 && c.green() < 80) {
                    iconL = std::min(iconL, x);
                    iconR = std::max(iconR, x);
                    row = y;
                }
            }
            if (iconR >= 0) break;
        }
        QVERIFY2(iconR >= 0, "the welcome tab painted no icon at all");

        // Find the painted plate on the icon's row: the tab background is
        // lighter than the bar behind it. Using the paint rather than
        // tabRect() keeps the assertion independent of the margin.
        const QRect tr = bar->tabRect(0);
        int plateL = -1, plateR = -1;
        for (int x = tr.left(); x <= tr.right() && x < shot.width(); ++x) {
            const QColor c = shot.pixelColor(x, row);
            const bool plate = (c.red() + c.green() + c.blue()) / 3 > 30;
            if (plate) { if (plateL < 0) plateL = x; plateR = x; }
        }
        QVERIFY2(plateL >= 0 && plateR > plateL, "could not find the tab plate");

        const int leftGap  = iconL - plateL;
        const int rightGap = plateR - iconR;
        QVERIFY2(qAbs(leftGap - rightGap) <= 1,
                 qPrintable(QString("icon not centred: %1px left vs %2px right "
                                    "(plate %3..%4, icon %5..%6)")
                                .arg(leftGap).arg(rightGap)
                                .arg(plateL).arg(plateR).arg(iconL).arg(iconR)));
    }

    // The welcome page must yield the view to an open app and take it back
    // when the last one closes. Qt does NOT do this for us: a non-current
    // tabified dock keeps its `visible` flag set and merely gets zero
    // geometry, so the page has to be hidden explicitly. Dropping that during
    // the tab rework is what broke the "opening an app replaces the welcome
    // page" integration test — and this asserts BOTH directions, because
    // hiding it was easy to get right while forgetting to bring it back.
    void welcomeVisibilityFollowsTheCurrentTab()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.resize(1200, 700);
        ws.show();
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QQuickWidget* page = ws.welcomePageWidget();
        QVERIFY(page != nullptr);
        QVERIFY2(page->isVisible(), "welcome page must show when nothing is open");

        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        QVERIFY2(!page->isVisible(),
                 "welcome page must yield to an app that has just opened");

        ws.removePluginDock("A");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        QVERIFY2(page->isVisible(),
                 "welcome page must return when the last app closes");
    }

    // The tabbed/side-by-side choice belongs to Ctrl+Shift+L alone. Qt's own
    // dock-drag would otherwise re-dock a plugin the moment its tab is dragged
    // out of the bar, silently switching layout mode. Docks stay closable so
    // the tab's x still works.
    void docksCannotBeDraggedOutOfTheTabBar()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.show();
        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        ws.addPluginDock(makePluginWidget("B"), "B", "App B");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        for (const QString& name : {QStringLiteral("A"), QStringLiteral("B")}) {
            QDockWidget* dock = ws.dockFor(name);
            QVERIFY2(dock != nullptr, qPrintable(name));
            QVERIFY2(!dock->features().testFlag(QDockWidget::DockWidgetMovable),
                     qPrintable(QString("dock '%1' is user-movable, so dragging "
                                        "its tab can change the layout mode")
                                    .arg(name)));
            QVERIFY2(!dock->features().testFlag(QDockWidget::DockWidgetFloatable),
                     qPrintable(QString("dock '%1' can be floated out").arg(name)));
            QVERIFY2(dock->features().testFlag(QDockWidget::DockWidgetClosable),
                     qPrintable(QString("dock '%1' must stay closable").arg(name)));
        }
    }

    // ...and the shortcut still does change it, so locking the drag has not
    // locked the feature.
    void layoutModeStillTogglesViaTheShortcut()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.show();
        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        ws.addPluginDock(makePluginWidget("B"), "B", "App B");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        const QString before = ws.layoutMode();
        QTest::keyClick(&ws, Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();

        QVERIFY2(ws.layoutMode() != before,
                 qPrintable(QString("layoutMode stayed '%1' after Ctrl+Shift+L")
                                .arg(before)));
    }

    // The welcome tab must stay narrow — it is icon-only. Its width comes from
    // a POSITIONAL stylesheet rule, and that rule failed to apply three
    // separate times (once because the bar was never styled at launch, once
    // because the hidden spacer took index 0, once because a single visible tab
    // matches :only-one rather than :first). Each time it showed up as a tab
    // several times too wide. welcomeTabIconIsCentred does catch it, but
    // reports "icon not centred", which sends you looking in the wrong place.
    void welcomeTabStaysNarrowerThanAppTabs()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.resize(1200, 700);
        ws.show();
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);

        // At launch the welcome tab is the ONLY visible one — the :only-one
        // case, which is exactly where this last regressed.
        const int soloWidth = bar->tabRect(0).width();
        QVERIFY2(soloWidth > 0 && soloWidth < 60,
                 qPrintable(QString("welcome tab is %1px wide at launch; the "
                                    "icon-only rule is not applying")
                                .arg(soloWidth)));

        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        int welcomeW = -1, appW = -1;
        for (int i = 0; i < bar->count(); ++i) {
            const QString t = bar->tabText(i);
            if (t.isEmpty()) welcomeW = bar->tabRect(i).width();
            else if (t == "App A") appW = bar->tabRect(i).width();
        }
        QVERIFY(welcomeW > 0 && appW > 0);
        QVERIFY2(welcomeW * 2 < appW,
                 qPrintable(QString("welcome tab (%1px) should be far narrower "
                                    "than an app tab (%2px)")
                                .arg(welcomeW).arg(appW)));
    }

    // The whole point of the tab: with an app open, clicking it returns you to
    // the welcome page — and clicking back returns you to the app. Nothing
    // covered this, which is how the page could stay hidden (or stay showing)
    // through several rounds of fixes without a test noticing.
    void clickingTheWelcomeTabReturnsToTheWelcomePage()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.resize(1200, 700);
        ws.show();
        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QQuickWidget* page = ws.welcomePageWidget();
        QVERIFY(page != nullptr);
        QVERIFY2(!page->isVisible(), "an open app should be showing first");

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        int welcomeTab = -1, appTab = -1;
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i).isEmpty()) welcomeTab = i;
            else if (bar->tabText(i) == "App A") appTab = i;
        }
        QVERIFY(welcomeTab >= 0 && appTab >= 0);

        bar->setCurrentIndex(welcomeTab);
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        QVERIFY2(page->isVisible(),
                 "clicking the welcome tab must bring the welcome page back");

        bar->setCurrentIndex(appTab);
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        QVERIFY2(!page->isVisible(),
                 "clicking back to the app must hide the welcome page again");
    }

    // Dragging the welcome tab is refused, but an APP tab could still be
    // dropped in front of it. That matters because the welcome tab's width
    // comes from a positional rule (:first) — with an app tab at index 0 the
    // app wears the icon-only 15px styling and the welcome tab wears the 120px
    // app styling. Asserts the order is restored AND that the close buttons
    // survive it, since the restoring moveTab destroys them.
    void appTabDraggedBeforeWelcomeIsPutBack()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.resize(1200, 700);
        ws.show();
        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        ws.addPluginDock(makePluginWidget("B"), "B", "App B");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QVERIFY2(bar->tabText(0).isEmpty(), "welcome tab should start at index 0");

        int appTab = -1;
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i) == "App A") { appTab = i; break; }
        }
        QVERIFY(appTab > 0);

        // Straight to the front — what a user dragging leftwards produces.
        bar->moveTab(appTab, 0);
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QVERIFY2(bar->tabText(0).isEmpty(),
                 qPrintable(QString("welcome tab must be restored to index 0, "
                                    "found '%1'").arg(bar->tabText(0))));

        for (int i = 0; i < bar->count(); ++i) {
            const QString t = bar->tabText(i);
            if (t.isEmpty() || t.startsWith("__")) continue;
            QVERIFY2(bar->tabButton(i, QTabBar::LeftSide) != nullptr,
                     qPrintable(QString("tab '%1' lost its close button when the "
                                        "order was restored").arg(t)));
        }
    }

    // Invariant: once the dust settles, the last thing activeAppChanged
    // reported names the open app — never "".
    //
    // HONEST LIMIT: this does not discriminate on x86_64. The bug it belongs to
    // (currentVisibleApp="" with a dock open) only reproduces on macOS and
    // aarch64, where Qt passes through a furniture tab on the way to the app's
    // tab and the old code emitted from the index the SIGNAL carried rather
    // than the settled one. Verified against a control build with the old
    // emission restored: it still passes here. Kept because the invariant is
    // real and CI runs the platforms where it bites.
    void transientFurnitureTabDoesNotClearTheActiveApp()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.resize(1200, 700);
        ws.show();
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        int appTab = -1, welcomeTab = -1;
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i) == "App A") appTab = i;
            else if (bar->tabText(i).isEmpty()) welcomeTab = i;
        }
        QVERIFY(appTab >= 0 && welcomeTab >= 0);

        QSignalSpy active(&ws, &WorkspaceArea::activeAppChanged);

        // Both in one turn, exactly as dock activation does it.
        bar->setCurrentIndex(welcomeTab);
        bar->setCurrentIndex(appTab);
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QVERIFY2(!active.isEmpty(), "activeAppChanged never fired");
        const QString last = active.last().at(0).toString();
        QCOMPARE(last, QStringLiteral("A"));
    }

    // Guards the CAUSE rather than the symptom. QTabBar::moveTab destroys a
    // tab's buttons, so hideSpacerTab must never reorder: it hides the spacer
    // where Qt put it and leaves every index alone. Asserting on the buttons
    // directly does not work here — the destroy-then-recreate race settles
    // differently under QtTest than in a real app, so a symptom test passes
    // even with the reordering restored. The index is deterministic.
    void hidingTheSpacerDoesNotReorderTabs()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.show();
        for (int i = 0; i < 8; ++i) QCoreApplication::processEvents();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);

        int spacerIndexBefore = -1;
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i).startsWith("__")) { spacerIndexBefore = i; break; }
        }
        QVERIFY2(spacerIndexBefore >= 0, "spacer tab not found");

        ws.addPluginDock(makePluginWidget("A"), "A", "App A");
        for (int i = 0; i < 8; ++i) QCoreApplication::processEvents();

        int spacerIndexAfter = -1;
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i).startsWith("__")) { spacerIndexAfter = i; break; }
        }
        QCOMPARE(spacerIndexAfter, spacerIndexBefore);
    }

    // The welcome tab's width comes from a POSITIONAL stylesheet rule, so its
    // index is load-bearing: if the hidden spacer takes index 0 the rule lands
    // on a tab nobody sees and the welcome tab renders at the default width.
    void welcomeTabIsFirstAndSpacerIsLast()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QVERIFY2(bar->tabText(0).isEmpty(),
                 "the title-less welcome tab must hold index 0");
        bool spacerHidden = false;
        for (int i = 0; i < bar->count(); ++i) {
            if (!bar->isTabVisible(i)) { spacerHidden = true; break; }
        }
        QVERIFY2(spacerHidden, "the spacer's tab must be hidden");
    }

    // The spacer exists only to make QMainWindow draw a tab bar for a single
    // dock; it must never be visible, clickable, or draggable.
    void spacerTabIsHiddenAndInert()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY2(bar != nullptr,
                 "a tab bar must exist at launch, before any app is opened");

        int visible = 0;
        for (int i = 0; i < bar->count(); ++i) {
            if (!bar->isTabVisible(i)) {
                QVERIFY2(!bar->isTabEnabled(i), "hidden spacer must be disabled");
                continue;
            }
            ++visible;
        }
        QCOMPARE(visible, 1);   // just the welcome tab
    }

    // Dragging the welcome tab must be refused outright, not corrected after
    // the fact. Drives real mouse events rather than moveTab() so it exercises
    // QTabBar's own move machinery — the thing being suppressed.
    void welcomeTabCannotBeDraggedAway()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QVERIFY2(bar->tabText(0).isEmpty(), "welcome tab should start at index 0");

        const QPoint from = bar->tabRect(0).center();
        const QPoint to   = bar->tabRect(bar->count() - 1).center();

        // Asserting on tabMoved, not on the final index: the pin-back handler
        // restores index 0 either way, so a position check passes even with
        // the refusal removed and proves nothing. The move must never START.
        QSignalSpy moved(bar, &QTabBar::tabMoved);

        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, from);
        // Several steps: one jump can fall short of the drag threshold.
        for (int i = 1; i <= 4; ++i) {
            QTest::mouseMove(bar, from + (to - from) * i / 4);
        }
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, to);
        processDeferred();

        QCOMPARE(moved.count(), 0);
        QVERIFY2(bar->tabText(0).isEmpty(),
                 "welcome tab must still be at index 0 after a drag attempt");
    }

    // The counterpart: the same synthetic drag DOES move an app tab. Without
    // this, the test above could pass simply because the synthetic events never
    // reach QTabBar's drag machinery in an offscreen window.
    void appTabDragActuallyMoves()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        ws.addPluginDock(makePluginWidget("B"), "B");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QVERIFY(bar->count() >= 3);          // welcome + A + B (+ hidden spacer)

        // Locate app tabs by TEXT, not index: the hidden spacer occupies an
        // index of its own and its tabRect is zero-sized, so dragging from a
        // hard-coded index can silently drag nothing.
        QList<int> appTabs;
        for (int i = 0; i < bar->count(); ++i) {
            const QString t = bar->tabText(i);
            if (!t.isEmpty() && !t.startsWith("__") && bar->isTabVisible(i))
                appTabs << i;
        }
        QVERIFY2(appTabs.size() >= 2, "need two app tabs to drag between");

        QSignalSpy moved(bar, &QTabBar::tabMoved);
        const QPoint from = bar->tabRect(appTabs.at(0)).center();
        const QPoint to   = bar->tabRect(appTabs.at(1)).center();

        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, from);
        for (int i = 1; i <= 4; ++i) {
            QTest::mouseMove(bar, from + (to - from) * i / 4);
        }
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, to);

        QVERIFY2(moved.count() > 0,
                 "an app tab drag must reach QTabBar — otherwise the welcome "
                 "tab's drag test is vacuous");
    }

    // The app tabs keep their reordering — the refusal above must be scoped to
    // the welcome tab, not implemented by turning movable off for the bar.
    void appTabsRemainDraggable()
    {
        QObject stubBackend;
        WorkspaceArea ws(&stubBackend);
        ws.addPluginDock(makePluginWidget("A"), "A");
        processDeferred();

        QTabBar* bar = tabBarOf(ws);
        QVERIFY(bar != nullptr);
        QVERIFY2(bar->isMovable(),
                 "app tabs must still be reorderable");
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
