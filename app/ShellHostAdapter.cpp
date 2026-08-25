#include "ShellHostAdapter.h"

#include "MainUIBackend.h"

#include <QWidget>

ShellHostAdapter::ShellHostAdapter(MainUIBackend* backend, QObject* parent)
    : QObject(parent)
    , m_backend(backend)
{
    if (!m_backend) {
        qFatal("ShellHostAdapter requires a MainUIBackend instance");
    }

    // Signal → observer translation, guarded on m_observer: the shell detaches
    // in destroyShell(), but PluginLoader's QTimer::singleShot(0, ...) and a 30s
    // ViewModuleHost timeout can still deliver a backend signal afterwards.
    connect(m_backend, &MainUIBackend::currentActiveSectionIndexChanged, this, [this]() {
        if (!m_observer || !m_backend) return;
        m_observer->onSectionIndexChanged(m_backend->currentActiveSectionIndex());
    });

    connect(m_backend, &MainUIBackend::navigateToApps, this, [this]() {
        if (!m_observer) return;
        m_observer->onNavigateToApps();
    });

    connect(m_backend, &MainUIBackend::pluginWindowRequested, this,
            [this](QWidget* widget, const QString& title) {
        if (!m_observer) return;
        m_observer->onPluginWindowRequested(widget, title);
    });

    connect(m_backend, &MainUIBackend::pluginWindowRemoveRequested, this,
            [this](QWidget* widget) {
        if (!m_observer) return;
        m_observer->onPluginWindowRemoveRequested(widget);
    });

    connect(m_backend, &MainUIBackend::pluginWindowActivateRequested, this,
            [this](QWidget* widget) {
        if (!m_observer) return;
        m_observer->onPluginWindowActivateRequested(widget);
    });
}

ShellHostAdapter::~ShellHostAdapter() = default;

QObject* ShellHostAdapter::backendObject()
{
    return m_backend.data();
}

int ShellHostAdapter::currentSectionIndex() const
{
    return m_backend ? m_backend->currentActiveSectionIndex() : 0;
}

void ShellHostAdapter::setCurrentSectionIndex(int index)
{
    if (m_backend) m_backend->setCurrentActiveSectionIndex(index);
}

void ShellHostAdapter::loadUiModule(const QString& name)
{
    if (m_backend) m_backend->loadUiModule(name);
}

void ShellHostAdapter::unloadUiModule(const QString& name)
{
    if (m_backend) m_backend->unloadUiModule(name);
}

void ShellHostAdapter::setCurrentVisibleApp(const QString& name)
{
    if (m_backend) m_backend->setCurrentVisibleApp(name);
}

QString ShellHostAdapter::displayNameFor(const QString& name) const
{
    return m_backend ? m_backend->displayNameFor(name) : name;
}

void ShellHostAdapter::setObserver(IShellObserver* observer)
{
    m_observer = observer;
}
