#include "SchemeRegistrar.h"

#include "LinkUrl.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace {

// Read by VALUE, not by presence.
//
// qEnvironmentVariableIsSet alone made `LOGOS_NO_SCHEME_REGISTER=0` skip
// registration — the exact opposite of what anyone writing that means. A flag
// whose "off" switch turns it on is worse than no flag, because it fails in the
// direction that is hardest to notice: links quietly stop working and the
// variable you set to fix it is the thing breaking it.
//
// Recognised as OFF: unset, empty, 0, false, no, off. Anything else is ON, so
// the documented `=1` and the natural `=true` / `=yes` all work.
bool skipRequested()
{
    if (!qEnvironmentVariableIsSet("LOGOS_NO_SCHEME_REGISTER"))
        return false;
    return SchemeRegistrar::valueMeansSkip(
        qEnvironmentVariable("LOGOS_NO_SCHEME_REGISTER"));
}

#ifdef Q_OS_LINUX
// The path the OS should launch.
QString launcherPath()
{
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty() && QFile::exists(appImage))
        return appImage;
    return QCoreApplication::applicationFilePath();
}

bool writeIfChanged(const QString& path, const QByteArray& content)
{
    QFile existing(path);
    if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
        const QByteArray current = existing.readAll();
        existing.close();
        if (current == content)
            return true;   // already correct; do not touch mtime
    }

    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return out.write(content) == content.size();
}
#endif

} // namespace

namespace SchemeRegistrar {

bool valueMeansSkip(const QString& value)
{
    const QString v = value.trimmed().toLower();

    // Empty counts as off: `export LOGOS_NO_SCHEME_REGISTER=` is what someone
    // types to clear it, and reading that as "on" would reintroduce the same
    // trap from the other side.
    return !(v.isEmpty()
             || v == QLatin1String("0")
             || v == QLatin1String("false")
             || v == QLatin1String("no")
             || v == QLatin1String("off"));
}

bool registerScheme()
{
    if (skipRequested()) {
        // A legitimate success — nothing is wrong and nothing was attempted —
        // but visible for the same reason as the #else below: "links do
        // nothing" is otherwise indistinguishable from a bug, and this is the
        // one cause a developer can fix by unsetting a variable.
        qInfo().noquote()
            << "SchemeRegistrar: LOGOS_NO_SCHEME_REGISTER="
            << qEnvironmentVariable("LOGOS_NO_SCHEME_REGISTER")
            << "— skipping registration;" << LinkUrl::scheme()
            << "links will not open this build. Unset it, or set it to 0, to"
               " register normally.";
        return true;
    }

#if defined(Q_OS_MACOS)
    // CFBundleURLTypes in Info.plist does this, and LaunchServices reads it
    // when the bundle is first launched. Nothing to write at runtime.
    //
    // Worth knowing while developing: every nix build produces a bundle at a
    // fresh /nix/store path, and LaunchServices keeps registrations keyed on
    // path, so a stale one can win. `lsregister -f <bundle>` re-points it.
    return true;

#elif defined(Q_OS_LINUX)
    const QString exec = launcherPath();
    if (exec.isEmpty()) {
        qWarning() << "SchemeRegistrar: cannot determine the executable path";
        return false;
    }

    const QString applications =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/applications");
    const QString path = applications + QStringLiteral("/logos-basecamp.desktop");

    // %u is what makes the OS pass the URL. Without it the app launches with no
    // argv and the click appears to do nothing — the failure this is easiest to
    // ship by accident, because the app does start.
    //
    // --uri=%u, not a bare %u, because main() reads the URL from that option
    // and never from a positional argument: the value arrives from a scheme
    // handler and is attacker-influenced, so one that happened to look like a
    // flag must not be read as one. A bare %u is the same class of failure as
    // omitting it — the app starts, argv carries the URL, and nothing consumes
    // it. The Windows command below is already this shape.
    const QByteArray entry =
        QStringLiteral("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=Logos Basecamp\n"
                       "Exec=\"%1\" --uri=%u\n"
                       "Icon=logos-basecamp\n"
                       "Terminal=false\n"
                       "Categories=Utility;\n"
                       "MimeType=x-scheme-handler/%2;\n")
            .arg(exec, LinkUrl::scheme())
            .toUtf8();

    if (!writeIfChanged(path, entry)) {
        qWarning().noquote() << "SchemeRegistrar: could not write" << path;
        return false;
    }

    // Refreshes the MIME cache. Absent on minimal systems, and the entry alone
    // is enough for several desktops, so a missing tool is not a failure.
    QProcess::startDetached(QStringLiteral("update-desktop-database"),
                            { applications });
    return true;

#elif defined(Q_OS_WIN)
    const QString exec = QDir::toNativeSeparators(
        QCoreApplication::applicationFilePath());
    if (exec.isEmpty())
        return false;

    // HKCU, not HKCR: there is no installer, and per-user needs no elevation.
    // QSettings rather than <windows.h> so this stays plain Qt and builds under
    // the mingw cross target unchanged.
    const QString root = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\")
                       + LinkUrl::scheme();

    QSettings key(root, QSettings::NativeFormat);
    key.setValue(QStringLiteral("Default"),
                 QStringLiteral("URL:%1 Protocol").arg(LinkUrl::scheme()));
    // The presence of this empty value is what marks the key as a URL protocol
    // handler. Its content is ignored.
    key.setValue(QStringLiteral("URL Protocol"), QString());

    QSettings icon(root + QStringLiteral("\\DefaultIcon"), QSettings::NativeFormat);
    icon.setValue(QStringLiteral("Default"), QStringLiteral("\"%1\",0").arg(exec));

    QSettings command(root + QStringLiteral("\\shell\\open\\command"),
                      QSettings::NativeFormat);
    // --uri= as an explicit option rather than a bare positional: the URL is
    // attacker-influenced, and one that happened to look like a flag must not
    // be read as one. The trailing %1 is the SHELL's placeholder, substituted
    // by Windows with the clicked URL — not a QString one, hence the manual
    // concatenation rather than another .arg().
    command.setValue(QStringLiteral("Default"),
                     QLatin1Char('"') + exec
                         + QStringLiteral("\" \"--uri=%1\""));

    command.sync();
    return command.status() == QSettings::NoError;

#else
    // NO IMPLEMENTATION FOR THIS PLATFORM, and saying so is the point.
    //
    // Returning true here would report a registration that never happened. The
    // failure it hides is the worst-shaped one this feature has: a link that
    // silently does nothing looks exactly like a malformed URL, a stale handler
    // or a browser that swallowed the click, and none of those leave a trace.
    // One warning at startup is the difference between an afternoon and a
    // minute.
    //
    // iOS specifically is NOT covered by this file, and not merely unfinished:
    // qt-ios/ builds a separate app image that does not compile app/ at all,
    // and iOS delivers URLs through QDesktopServices::setUrlHandler rather than
    // QFileOpenEvent. It needs its own registration AND its own delivery path;
    // only the parser and the coordinator would carry over unchanged.
    qWarning().noquote()
        << "SchemeRegistrar: no URL-scheme registration exists for this "
           "platform —" << LinkUrl::scheme() << "links will not reach this build.";
    return false;
#endif
}

} // namespace SchemeRegistrar
