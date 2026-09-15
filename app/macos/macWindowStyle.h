#ifndef MACWINDOWSTYLE_H
#define MACWINDOWSTYLE_H

class QMainWindow;

void applyMacWindowRoundedCorners(QMainWindow* w, bool rounded = true);

// Brings the app to the front; raise()/activateWindow() can't on macOS.
// Not enough on its own from the background: since macOS 14 `[NSApp activate]`
// is ignored unless another process has yielded activation first.
void macActivateApp();

// The other half of that. Called by a second Basecamp launch on the instance
// already running — the secondary is the process the user just started, so it
// is the one macOS will listen to.
void macYieldActivationTo(long long pid);

// True after Cmd+H: an AppKit app-hide leaves Qt widget visibility untouched.
bool macAppIsHidden();

// Takes the window out of the Dock, for when clearing Qt::WindowMinimized
// doesn't reach NSWindow (e.g. it was ordered out while still miniaturized).
void macDeminiaturize(QMainWindow* w);

#endif // MACWINDOWSTYLE_H
