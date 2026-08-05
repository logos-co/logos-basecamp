#ifndef MACWINDOWSTYLE_H
#define MACWINDOWSTYLE_H

class QMainWindow;

void applyMacWindowRoundedCorners(QMainWindow* w, bool rounded = true);

// Brings the app to the front; raise()/activateWindow() can't on macOS.
void macActivateApp();

// True after Cmd+H: an AppKit app-hide leaves Qt widget visibility untouched.
bool macAppIsHidden();

// For when clearing Qt::WindowMinimized doesn't reach NSWindow.
void macDeminiaturize(QMainWindow* w);

#endif // MACWINDOWSTYLE_H
