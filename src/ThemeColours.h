#pragma once

#include <ThemeStore.h>

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QQuickWidget>
#include <QString>
#include <QWidget>

#include <functional>

// The current theme's colours for the shell's widget chrome (QSS, QPalette,
// clear colours), which no QML binding reaches. Logos::ThemeStore resolves
// them exactly as it does Theme.palette.
namespace ThemeColours {

inline QColor color(const char* token)
{
    return Logos::ThemeStore::instance()->color(QLatin1String(token));
}

// `token` as a QSS colour, its alpha scaled by `alpha`.
inline QString css(const char* token, qreal alpha = 1.0)
{
    const QColor c = color(token);
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(c.alphaF() * alpha, 0, 'f', 3);
}

// Runs `apply` now and on every palette change, until `context` is destroyed.
inline void bind(QObject* context, const std::function<void()>& apply)
{
    apply();
    QObject::connect(Logos::ThemeStore::instance(), &Logos::ThemeStore::paletteChanged,
                     context, apply);
}

inline void clearToBackground(QQuickWidget* widget)
{
    bind(widget, [widget] { widget->setClearColor(color("background")); });
}

inline void fillWithBackground(QWidget* widget)
{
    widget->setAutoFillBackground(true);
    bind(widget, [widget] {
        QPalette p = widget->palette();
        p.setColor(QPalette::Window, color("background"));
        widget->setPalette(p);
    });
}

} // namespace ThemeColours
