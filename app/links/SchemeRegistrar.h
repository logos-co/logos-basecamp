#pragma once

#include <QString>

// ── SchemeRegistrar ──────────────────────────────────────────────────────────
//
// Tells the OS that `basecamp://` means this executable. That registration is
// what makes the COLD path work at all: with nothing running, the OS looks up a
// command line and launches it. The single-instance socket only ever covers the
// warm path.
//
// Per platform:
//
//   macOS   — nothing to do here. CFBundleURLTypes in Info.plist is the whole
//             mechanism; LaunchServices registers the bundle on first launch and
//             delivers to the live process as a QFileOpenEvent.
//   Linux   — a desktop entry with MimeType=x-scheme-handler/basecamp and
//             `--uri=%u`, attached to the option main() reads and never a
//             bare positional. The one shipped inside the AppImage is never
//             seen by the system, so it is written to
//             ~/.local/share/applications on first run.
//   Windows — HKCU\Software\Classes\basecamp. There is no installer, and HKCU
//             needs no elevation.
// Idempotent: rewrites only when the content would change, so it is safe to
// call on every launch.
//
// LOGOS_NO_SCHEME_REGISTER skips it entirely. CI and doc-tests build many
// throwaway apps at fresh store paths and must not fight over the machine's
// real basecamp:// handler — the last one to run would win, pointing at a path
// that may since have been garbage-collected.
//
// Read by VALUE: unset, empty, `0`, `false`, `no` and `off` all mean "register
// normally"; anything else (`1`, `true`, `yes`) skips. Presence alone used to
// be enough, which made `=0` do the opposite of what it says — a flag whose
// off switch turns it on, failing in the direction hardest to notice.
namespace SchemeRegistrar {

bool registerScheme();
bool valueMeansSkip(const QString& value);

} // namespace SchemeRegistrar
