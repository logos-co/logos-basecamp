// The Qt window-state semantics Window::isWindowShown()/restoreWindow() rely
// on (issue #268). The toggle itself is covered by tests/ui-tests.mjs, which
// drives the real Window; only Qt's own behaviour is pinned here.
//
//   nix build .#unit-tests -L
#include <QtTest/QtTest>
#include <QApplication>
#include <QMainWindow>

class WindowStateTest : public QObject
{
    Q_OBJECT

private slots:
    void minimizedWindowStaysVisibleToQt();
    void clearingMinimizedRestoresPreviousState();
};

void WindowStateTest::minimizedWindowStaysVisibleToQt()
{
    QMainWindow w;
    w.show();
    w.showMinimized();

    QVERIFY2(w.isMinimized(), "showMinimized() should set the minimized state");
    QVERIFY2(w.isVisible(),
        "Qt keeps isVisible() true for a minimized window — the trap behind #268");
}

void WindowStateTest::clearingMinimizedRestoresPreviousState()
{
    QMainWindow w;
    w.showMaximized();
    QVERIFY(w.isMaximized());

    w.showMinimized();
    QVERIFY(w.isMinimized());

    // What restoreWindow() does: clear only the bit, so maximized survives.
    w.setWindowState(w.windowState() & ~Qt::WindowMinimized);

    QVERIFY(!w.isMinimized());
    QVERIFY2(w.isMaximized(),
        "Restoring from the tray must preserve maximized/fullscreen");
}

QTEST_MAIN(WindowStateTest)
#include "window_state_test.moc"
