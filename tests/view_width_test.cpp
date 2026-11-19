#include <QtTest>
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTemporaryDir>

#include <memory>

#include "ViewWidth.h"

// An app view's Logos.Theme is told how wide the view is, and follows it.
// A stand-in Logos.Theme plays the design system's: the same singleton name
// and the same viewWidth property, which is the whole contract.
class ViewWidthTest : public QObject
{
    Q_OBJECT

    static bool write(const QString& path, const QByteArray& content)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
    }

    static std::unique_ptr<QQuickItem> createView(QQmlEngine& engine, const QByteArray& qml)
    {
        QQmlComponent component(&engine);
        component.setData(qml, QUrl());
        auto* item = qobject_cast<QQuickItem*>(component.create());
        if (!item)
            qWarning() << component.errors();
        return std::unique_ptr<QQuickItem>(item);
    }

private slots:
    void themeFollowsTheViewWidth()
    {
        QTemporaryDir modules;
        QVERIFY(QDir(modules.path()).mkpath("Logos/Theme"));
        QVERIFY(write(modules.filePath("Logos/Theme/qmldir"),
                      "module Logos.Theme\nsingleton Theme 1.0 Theme.qml\n"));
        QVERIFY(write(modules.filePath("Logos/Theme/Theme.qml"),
                      "pragma Singleton\nimport QtQuick\nQtObject { property real viewWidth: 0 }\n"));

        QQmlEngine engine;
        engine.addImportPath(modules.path());
        auto view = createView(engine,
            "import QtQuick\nimport Logos.Theme\nItem { property real seen: Theme.viewWidth }\n");
        QVERIFY(view);

        view->setWidth(390);
        followViewWidth(&engine, view.get());
        QCOMPARE(view->property("seen").toReal(), 390.0);

        view->setWidth(1024);
        QCOMPARE(view->property("seen").toReal(), 1024.0);
    }

    // An app that doesn't use the design system is left as it is.
    void viewWithoutLogosThemeIsLeftAlone()
    {
        QQmlEngine engine;
        auto view = createView(engine, "import QtQuick\nItem { width: 390 }\n");
        QVERIFY(view);
        followViewWidth(&engine, view.get());
        QCOMPARE(view->width(), 390.0);
    }
};

QTEST_MAIN(ViewWidthTest)
#include "view_width_test.moc"
