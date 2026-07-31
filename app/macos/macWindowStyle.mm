#include <QtCore/qglobal.h>
#ifdef Q_OS_MAC
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#endif

#include "macWindowStyle.h"
#include <QMainWindow>
#include <QGuiApplication>

void applyMacWindowRoundedCorners(QMainWindow* w, bool rounded)
{
#ifdef Q_OS_MAC
    if (QGuiApplication::platformName() == "offscreen") return;
    if (!w) return;
    NSView* nsView = (NSView*)w->winId();
    if (!nsView) return;

    nsView.wantsLayer = YES;
    if (nsView.layer) {
        nsView.layer.cornerRadius = rounded ? 10.0 : 0.0;
        nsView.layer.masksToBounds = rounded;
        nsView.layer.borderWidth = rounded ? 0.5 : 0.0;
        nsView.layer.borderColor = rounded ? [NSColor separatorColor].CGColor : nullptr;
    }
#endif
}

void macActivateApp()
{
#ifdef Q_OS_MAC
    if (QGuiApplication::platformName() == "offscreen") return;
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];
#endif
}

bool macAppIsHidden()
{
#ifdef Q_OS_MAC
    if (QGuiApplication::platformName() == "offscreen") return false;
    return [NSApp isHidden];
#else
    return false;
#endif
}

void macDeminiaturize(QMainWindow* w)
{
#ifdef Q_OS_MAC
    if (QGuiApplication::platformName() == "offscreen") return;
    if (!w) return;
    NSView* nsView = (NSView*)w->winId();
    NSWindow* nsWindow = nsView ? nsView.window : nil;
    // Qt's window-state handling can miss this after an ordered-out hide.
    if (nsWindow && nsWindow.isMiniaturized)
        [nsWindow deminiaturize:nil];
#else
    Q_UNUSED(w);
#endif
}
