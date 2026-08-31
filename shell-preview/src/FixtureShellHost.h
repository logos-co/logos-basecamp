#pragma once

#include "IShellHost.h"
#include "FixtureBackend.h"

#include <QJsonObject>

// IShellHost backed by a fixture.
//
// The shell (main_ui.so) links no Logos and talks to the host only through this
// vtable, so a host that never mentions Logos runs the real, shipped UI.
//
// Plugin loading is out of scope: loadUiModule/unloadUiModule log and return.
// PluginLoader lives on the host side, and reinstating it would pull in
// LogosAPI, ui-host and the whole runtime this exists to avoid.
class FixtureShellHost : public IShellHost {
public:
    explicit FixtureShellHost(const QJsonObject& fixture);

    QObject* backendObject() override;
    int      currentSectionIndex() const override;
    void     setCurrentSectionIndex(int index) override;
    void     loadUiModule(const QString& name) override;
    void     unloadUiModule(const QString& name) override;
    void     setCurrentVisibleApp(const QString& name) override;
    QString  displayNameFor(const QString& name) const override;
    void     setObserver(IShellObserver* observer) override;

private:
    FixtureBackend  m_backend;
    IShellObserver* m_observer = nullptr;
};
