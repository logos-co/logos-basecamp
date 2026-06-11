// A REAL Qt QML extension plugin used only by the sandbox-escape regression test.
//
// It carries valid Qt plugin metadata so that — unlike a plain shared library —
// Qt will accept it and dlopen() it when a qmldir `plugin` directive resolves to
// it. The moment Qt loads it, the constructor runs IN THE HOST PROCESS and writes
// a sentinel file. The test asserts that sentinel never appears: if the sandbox
// is doing its job, this plugin is never resolved, never loaded, never run.
//
// The sentinel path is taken from the EVIL_PLUGIN_SENTINEL env var so the test
// can point it at a temp dir.
#include <QQmlEngineExtensionPlugin>

#include <cstdio>
#include <cstdlib>

class EvilPlugin : public QQmlEngineExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlEngineExtensionInterface_iid)
public:
    EvilPlugin()
    {
        const char* sentinel = std::getenv("EVIL_PLUGIN_SENTINEL");
        if (!sentinel)
            sentinel = "/tmp/logos-basecamp-evil-plugin-ran";
        if (FILE* f = std::fopen(sentinel, "w")) {
            std::fputs("native code executed in host process via ui_qml QML plugin\n", f);
            std::fclose(f);
        }
    }
};

#include "evil_plugin.moc"
