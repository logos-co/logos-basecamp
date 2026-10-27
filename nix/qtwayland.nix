# qt6.qtwayland as Basecamp builds it (RFC-4 rev 2 H1): the nested compositor
# in app/surface/ reads client wl_shm pools through Qt's SharedMemoryBuffer,
# and stock qtwayland 6.9.2 does so without wl_shm_buffer_begin_access /
# end_access, so a module that truncates its pool after committing would take
# the host's render thread down with SIGBUS. The patch adds the guard; it is
# also offered upstream. The client side (the wayland QPA plugin ui-host
# uses) is unchanged by it.
#
# Linux only; callers use `pkgs.qt6.qtwayland` as-is elsewhere.
{ pkgs }:

if pkgs.stdenv.isLinux then
  pkgs.qt6.qtwayland.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./patches/qtwayland-shm-guarded-access.patch ];
  })
else
  pkgs.qt6.qtwayland
