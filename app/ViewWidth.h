#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QQuickItem>

// Tell an app view's Logos.Theme how wide the view is, so the app's layouts
// and touch sizes follow it (phone, tablet, desktop). This is the design
// system's ThemeManager::followViewWidth in plain Qt: the design system is
// linked into the shell plugin, and a second copy in this binary would get a
// theme store of its own.
inline void followViewWidth(QQmlEngine* engine, QQuickItem* root)
{
    QObject* theme = engine && root
        ? engine->singletonInstance<QObject*>("Logos.Theme", "Theme") : nullptr;
    if (!theme)
        return;
    const auto apply = [root, theme] { theme->setProperty("viewWidth", root->width()); };
    QObject::connect(root, &QQuickItem::widthChanged, theme, apply);
    apply();
}
