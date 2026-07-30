#include "ShortcutBridge.h"

#include <QDebug>
#include <QKeySequence>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickWidget>
#include <QShortcut>
#include <QStackedWidget>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QWidget>

namespace {
// Substring match — QQuickShortcut lives in QtQuick and its moc'd class
// name is stable across Qt 6.x variants.
constexpr auto kShortcutClass = "QQuickShortcut";
} // namespace

ShortcutBridge::ShortcutBridge(QWidget* host,
                               QStackedWidget* stack,
                               DockPaneProvider dockProvider)
    : QObject(host)
    , m_host(host)
    , m_stack(stack)
    , m_dockProvider(std::move(dockProvider))
{
    if (m_stack) {
        connect(m_stack, &QStackedWidget::currentChanged,
                this, &ShortcutBridge::rebindDeferred);
    }
    rebindDeferred();
}

void ShortcutBridge::rebindDeferred()
{
    // Queued: currentChanged often fires mid-swap and the new pane's
    // QML may still be Loading — let the frame settle before scanning.
    QMetaObject::invokeMethod(this, &ShortcutBridge::rebind,
                              Qt::QueuedConnection);
}

void ShortcutBridge::rebind()
{
    clearMirrors();

    QWidget* current = m_stack ? m_stack->currentWidget() : nullptr;
    if (!current) return;

    if (auto* qw = qobject_cast<QQuickWidget*>(current)) {
        scanPane(qw);
        return;
    }
    // Current widget hosts a QQuickWidget rather than being one (e.g.
    // WorkspaceArea → active dock). Ask the host for it.
    if (m_dockProvider) {
        if (QQuickWidget* dockPane = m_dockProvider())
            scanPane(dockPane);
    }
}

bool ShortcutBridge::scanPane(QQuickWidget* pane)
{
    if (!pane) return false;

    // QML still loading — retry when Ready, otherwise findChildren would
    // return empty and we'd silently miss all its shortcuts.
    if (pane->status() != QQuickWidget::Ready) {
        m_transientConns.append(
            connect(pane, &QQuickWidget::statusChanged,
                    this, [this](QQuickWidget::Status s) {
                        if (s == QQuickWidget::Ready) rebindDeferred();
                    }));
        return false;
    }

    QQuickItem* root = pane->rootObject();
    if (!root) return false;

    // findChildren traverses both QQuickItem child items and their
    // attached QObject children — where QML `Shortcut { }` lives.
    for (QObject* obj : root->findChildren<QObject*>()) {
        if (!obj) continue;
        const QString cls = QString::fromUtf8(obj->metaObject()->className());
        if (!cls.contains(QLatin1String(kShortcutClass))) continue;
        mirrorOneShortcut(obj);
    }

    if (!m_mirrors.isEmpty()) {
        qDebug() << "ShortcutBridge: bound" << m_mirrors.size()
                 << "QML shortcut(s) on pane"
                 << pane->metaObject()->className();
    }
    return true;
}

void ShortcutBridge::mirrorOneShortcut(QObject* obj)
{
    // Guards against a future Qt class whose name coincidentally
    // contains "QQuickShortcut" but isn't one.
    if (obj->metaObject()->indexOfSignal("activated()") < 0) return;

    // Widget-scoped shortcuts are deliberately item-local — don't
    // silently promote them to pane-wide via a mirror.
    const int contextVal = obj->property("context").toInt();
    if (contextVal == Qt::WidgetShortcut ||
        contextVal == Qt::WidgetWithChildrenShortcut) {
        return;
    }

    // QML shortcuts declare either `sequence` (single) or `sequences`
    // (list of alternatives) — mirror each so any of them fires.
    QVariantList sequences;
    const QVariant sequencesVar = obj->property("sequences");
    if (sequencesVar.isValid() && sequencesVar.canConvert<QVariantList>()) {
        sequences = sequencesVar.toList();
    }
    if (sequences.isEmpty()) {
        const QVariant seqVar = obj->property("sequence");
        if (seqVar.isValid()) sequences.append(seqVar);
    }

    for (const QVariant& seqVar : sequences) {
        const QKeySequence seq(seqVar.toString());
        if (seq.isEmpty()) continue;

        // ApplicationShortcut: we've already scoped by "this pane is
        // current" via mirror lifetime, so widget/window scoping would
        // just re-introduce the offscreen-window problem.
        auto* mirror = new QShortcut(seq, m_host);
        mirror->setContext(Qt::ApplicationShortcut);
        mirror->setEnabled(obj->property("enabled").toBool());
        // UniqueConnection makes this loop safe on two axes:
        //   * a QML Shortcut can declare multiple sequences (each mirrored)
        //     — the enabledChanged→slot wire should still be one connection
        //     per QML shortcut, not one per sequence.
        //   * rebind() clears the C++ mirrors but leaves the QML shortcuts
        //     alive (they belong to the pane's QML tree). The next scan
        //     would re-connect the same signal, and without this flag
        //     onQmlShortcutEnabledChanged() would fire N times per change
        //     after N pane switches.
        connect(obj, SIGNAL(enabledChanged()),
                this, SLOT(onQmlShortcutEnabledChanged()),
                Qt::UniqueConnection);
        m_mirrorToQml.insert(mirror, obj);
        m_qmlToMirrors.insert(obj, QPointer<QShortcut>(mirror));

        // Three-way wiring covers Qt's ambiguity cycling: on platforms
        // where the QML shortcut also matches, Qt alternates
        // `activatedAmbiguously` between the two sides — hooking both
        // ends means the handler always runs on the first press.
        // String signals: QQuickShortcut has no public header.
        connect(mirror, SIGNAL(activated()),
                this,   SLOT(onWiredShortcut()));
        connect(mirror, SIGNAL(activatedAmbiguously()),
                this,   SLOT(onWiredShortcut()));
        connect(obj,    SIGNAL(activatedAmbiguously()),
                this,   SLOT(onWiredShortcut()));
        m_mirrors.append(mirror);
    }
}

void ShortcutBridge::onQmlShortcutEnabledChanged()
{
    QObject* qml = sender();
    if (!qml) return;
    const bool enabled = qml->property("enabled").toBool();
    for (auto it = m_qmlToMirrors.constFind(qml);
         it != m_qmlToMirrors.constEnd() && it.key() == qml; ++it) {
        if (QShortcut* m = it.value()) m->setEnabled(enabled);
    }
}

void ShortcutBridge::clearMirrors()
{
    for (QPointer<QShortcut> sc : m_mirrors) {
        if (sc) sc->deleteLater();
    }
    m_mirrors.clear();
    m_mirrorToQml.clear();
    m_qmlToMirrors.clear();

    // Drop pending statusChanged hooks so the old pane's late-Ready
    // signal doesn't retrigger a rebind after we've moved on.
    for (const QMetaObject::Connection& c : m_transientConns) disconnect(c);
    m_transientConns.clear();
}

void ShortcutBridge::onWiredShortcut()
{
    QObject* src = sender();
    if (!src) return;

    // Mirror → its QML target; QML shortcut itself → use directly.
    QPointer<QObject> qml = m_mirrorToQml.value(src);
    if (!qml) qml = src;
    if (!qml) return;

    // Check enabled at fire time so `enabled: someBinding` still works.
    if (!qml->property("enabled").toBool()) return;

    QMetaObject::invokeMethod(qml.data(), "activated",
                              Qt::DirectConnection);
}
