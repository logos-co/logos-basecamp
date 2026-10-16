# Generates build-info.json (version + commit hashes). nix/app.nix stages it
# as buildinfo/ beside bin/, and app/utils/BuildInfo.h reads it at startup,
# so input bumps restage a file instead of recompiling the app.
{ pkgs, buildInfo }:

pkgs.writeText "build-info.json" (builtins.toJSON {
  inherit (buildInfo) version commits;
})
