// srcdeps: ShortcutBridge.cpp
//
// Unit tests for ShortcutBridge. Uses inline QML in a QQuickWidget as a
// stand-in for plugin panes. Run via `nix build .#unit-tests -L`.

#include "ShortcutBridge.h"

#include <QtTest/QtTest>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QShortcut>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace {

// Loads inline QML into a QQuickWidget and returns the widget.
QQuickWidget* makePane(const QString& qml, QWidget* parent = nullptr)
{
    auto* qw = new QQuickWidget(parent);
    qw->setResizeMode(QQuickWidget::SizeRootObjectToView);
    qw->setSource(QUrl(QStringLiteral("data:text/plain,")
                       + qml.toUtf8().toPercentEncoding()));
    QTest::qWait(50);
    return qw;
}

QObject* findShortcutByObjectName(QQuickWidget* qw, const QString& name)
{
    if (!qw || !qw->rootObject()) return nullptr;
    return qw->rootObject()->findChild<QObject*>(name);
}

int mirrorCount(QWidget* host)
{
    return host->findChildren<QShortcut*>().size();
}

// Let the bridge's QueuedConnection rebind hop settle.
void pump()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

} // namespace

class ShortcutBridgeTest : public QObject {
    Q_OBJECT

private slots:
    // -----------------------------------------------------------------
    // Basic wiring
    // -----------------------------------------------------------------

    void mirrorsAreCreatedForCurrentPane()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { objectName: \"sc\"; sequence: \"Ctrl+K\" }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        QCOMPARE(mirrorCount(&host), 1);
    }

    void multipleShortcutsAllMirrored()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { sequence: \"Ctrl+K\" }\n"
            "  Shortcut { sequence: \"Ctrl+F\" }\n"
            "  Shortcut { sequence: \"Ctrl+P\" }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        QCOMPARE(mirrorCount(&host), 3);
    }

    void mirrorIsNamedAfterItsQmlShortcut()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { objectName: \"sc\"; sequence: \"Ctrl+K\" }\n"
            "  Shortcut { sequence: \"Ctrl+F\" }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        auto* named = host.findChild<QShortcut*>(QStringLiteral("shortcutBridge.mirror.sc"));
        QVERIFY(named);
        QCOMPARE(named->key(), QKeySequence(QStringLiteral("Ctrl+K")));
        // An unnamed QML shortcut leaves its mirror unnamed.
        QCOMPARE(mirrorCount(&host), 2);
        int unnamed = 0;
        for (QShortcut* sc : host.findChildren<QShortcut*>())
            if (sc->objectName().isEmpty()) ++unnamed;
        QCOMPARE(unnamed, 1);
    }

    void nonQuickPaneIsIgnored()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        stack->addWidget(new QLabel("plain widget", stack));
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        QCOMPARE(mirrorCount(&host), 0);
    }

    // -----------------------------------------------------------------
    // Fire dispatch
    // -----------------------------------------------------------------

    void mirrorActivatedInvokesQmlHandler()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  id: root\n"
            "  property int hits: 0\n"
            "  Shortcut { objectName: \"sc\"; sequence: \"Ctrl+K\";\n"
            "             onActivated: root.hits = root.hits + 1 }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        auto* mirror = host.findChild<QShortcut*>();
        QVERIFY(mirror);
        emit mirror->activated();
        pump();

        QCOMPARE(pane->rootObject()->property("hits").toInt(), 1);
    }

    // The mirror fires on the HOST, so without this the QML handler runs
    // while the pane still has no Qt keyboard focus. A handler that focuses
    // something then leaves the two halves of focus split: the item has
    // activeFocus inside the offscreen QQuickWindow and draws a caret, while
    // key events keep going to whichever widget actually holds focus. ⌘K on
    // the welcome page looked ready and swallowed every keystroke.
    //
    // focusWidget() rather than hasFocus(): the latter also requires an active
    // window, which a test run offscreen cannot rely on.
    void mirrorActivatedGivesThePaneKeyboardFocus()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        // Something else holds focus first, so passing is not just the pane
        // happening to be the only candidate.
        auto* other = new QLineEdit(&host);
        layout->addWidget(other);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { objectName: \"sc\"; sequence: \"Ctrl+K\" }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        other->setFocus();
        pump();
        QCOMPARE(host.focusWidget(), other);

        ShortcutBridge bridge(&host, stack);
        pump();

        auto* mirror = host.findChild<QShortcut*>();
        QVERIFY(mirror);
        emit mirror->activated();
        pump();

        QCOMPARE(host.focusWidget(), pane);
    }

    // A disabled QML shortcut is skipped entirely — including the focus hand-off
    // above, which must not fire for a handler that is never going to run.
    void disabledQmlShortcutDoesNotStealFocus()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* other = new QLineEdit(&host);
        layout->addWidget(other);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { objectName: \"sc\"; sequence: \"Ctrl+K\"; enabled: false }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        other->setFocus();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        auto* mirror = host.findChild<QShortcut*>();
        if (mirror) {
            emit mirror->activated();
            pump();
        }

        QCOMPARE(host.focusWidget(), other);
    }

    // macOS first-press case: Qt fires activatedAmbiguously on the QML
    // shortcut (registered first). Bridge must still run the handler.
    void qmlAmbiguityInvokesQmlHandler()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  id: root\n"
            "  property int hits: 0\n"
            "  Shortcut { objectName: \"sc\"; sequence: \"Ctrl+K\";\n"
            "             onActivated: root.hits = root.hits + 1 }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        QObject* qml = findShortcutByObjectName(pane, "sc");
        QVERIFY(qml);
        QMetaObject::invokeMethod(qml, "activatedAmbiguously");
        pump();

        QCOMPARE(pane->rootObject()->property("hits").toInt(), 1);
    }

    void disabledQmlShortcutIsNotFired()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  id: root\n"
            "  property int hits: 0\n"
            "  Shortcut { objectName: \"sc\"; sequence: \"Ctrl+K\";\n"
            "             enabled: false\n"
            "             onActivated: root.hits = root.hits + 1 }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        auto* mirror = host.findChild<QShortcut*>();
        QVERIFY(mirror);
        emit mirror->activated();
        pump();

        QCOMPARE(pane->rootObject()->property("hits").toInt(), 0);
    }

    // -----------------------------------------------------------------
    // Improvements #1-#4 from the design review
    // -----------------------------------------------------------------

    // Widget-scoped shortcuts stay item-local — must not be promoted
    // to pane-wide via a mirror.
    void widgetScopedShortcutsAreNotMirrored()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { sequence: \"Ctrl+A\"; context: Qt.WidgetShortcut }\n"
            "  Shortcut { sequence: \"Ctrl+B\"; context: Qt.WidgetWithChildrenShortcut }\n"
            "  Shortcut { sequence: \"Ctrl+K\"; context: Qt.WindowShortcut }\n"
            "  Shortcut { sequence: \"Ctrl+F\"; context: Qt.ApplicationShortcut }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        // Only the two pane-wide / app-wide ones get mirrors.
        QCOMPARE(mirrorCount(&host), 2);
    }

    // `Shortcut { sequences: [...] }` mirrors one QShortcut per alt.
    void multiSequenceShortcutIsFullyMirrored()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { sequences: [\"Ctrl+K\", \"Ctrl+L\"] }\n"
            "}\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        QCOMPARE(mirrorCount(&host), 2);
    }

    // -----------------------------------------------------------------
    // Pane switching
    // -----------------------------------------------------------------

    void paneSwitchReplacesMirrors()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* paneA = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { sequence: \"Ctrl+A\" }\n"
            "  Shortcut { sequence: \"Ctrl+B\" }\n"
            "}\n",
            stack);
        auto* paneB = makePane(
            "import QtQuick 2.15\n"
            "Item {\n"
            "  Shortcut { sequence: \"Ctrl+X\" }\n"
            "}\n",
            stack);
        stack->addWidget(paneA);
        stack->addWidget(paneB);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();
        QCOMPARE(mirrorCount(&host), 2);

        stack->setCurrentWidget(paneB);
        pump();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(mirrorCount(&host), 1);

        stack->setCurrentWidget(paneA);
        pump();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(mirrorCount(&host), 2);
    }

    void hiddenPaneShortcutsAreNotMirrored()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* visiblePane = makePane(
            "import QtQuick 2.15\n"
            "Item { Shortcut { sequence: \"Ctrl+V\" } }\n",
            stack);
        auto* hiddenPane = makePane(
            "import QtQuick 2.15\n"
            "Item { Shortcut { sequence: \"Ctrl+K\" } }\n",
            stack);
        stack->addWidget(visiblePane);
        stack->addWidget(hiddenPane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();

        const auto mirrors = host.findChildren<QShortcut*>();
        QCOMPARE(mirrors.size(), 1);
        QCOMPARE(mirrors.first()->key(), QKeySequence("Ctrl+V"));
    }

    // -----------------------------------------------------------------
    // Explicit rebind
    // -----------------------------------------------------------------

    void rebindDeferredPicksUpLateShortcuts()
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* stack = new QStackedWidget(&host);
        layout->addWidget(stack);

        auto* pane = makePane(
            "import QtQuick 2.15\n"
            "Item { objectName: \"root\" }\n",
            stack);
        stack->addWidget(pane);
        host.show();
        pump();

        ShortcutBridge bridge(&host, stack);
        pump();
        QCOMPARE(mirrorCount(&host), 0);

        // Splice a Shortcut in the way a Loader/dynamic Component would.
        QQmlEngine* eng = pane->engine();
        QQmlComponent comp(eng);
        comp.setData(
            "import QtQuick 2.15\n"
            "Shortcut { sequence: \"Ctrl+K\" }\n",
            QUrl("data:new-shortcut"));
        QVERIFY2(!comp.isError(), qPrintable(comp.errorString()));
        QObject* sc = comp.create(eng->rootContext());
        QVERIFY(sc);
        sc->setParent(pane->rootObject());

        bridge.rebindDeferred();
        pump();

        QCOMPARE(mirrorCount(&host), 1);
    }
};

QTEST_MAIN(ShortcutBridgeTest)
#include "shortcut_bridge_test.moc"
