# The modules an app under development needs, installed into one root for
# standalone mode's `--modules-dir`.
#
# `packages` come ready-built from the app's own flake evaluation — its
# `follows` and `--override-input` applied, which only that evaluation knows —
# and are installed as they are (lgpm), never rebuilt. They are the app's.
#
# Then walks dependencies breadth-first from `deps` (the app's own, in its order),
# then each of `apps`' (other apps opened next to it). One copy per module, and
# the first one reached wins: Basecamp loads the NEWEST of two copies, so a
# second one would silently replace the version the developer pinned. A
# module's dependencies are read from its flake's `config.dependencies`
# (logos-module-builder), else its metadata.json, and resolved through ITS
# flake inputs — so each comes at the revision its dependant's lock pins.
{ pkgs, system, lgpm ? null }:

{ packages ? []   # [ { name; package; } ] — .lgx packages, ready-built
, deps ? []       # [ { name; flake; } ] — dependencies to resolve from source
, apps ? []       # [ flake ] — other apps; their dependencies are added after
, exclude ? []    # names already provided (the apps themselves)
}:

let
  lib = pkgs.lib;

  depNamesOf = flake:
    let
      config = flake.configFor.${system} or flake.config or null;
      raw =
        if config != null then config.dependencies or []
        else (builtins.fromJSON (builtins.readFile "${flake}/metadata.json")).dependencies or [];
    in map (d: if builtins.isString d then d else d.name) raw;

  # `flake` stays unevaluated until the entry is reached, so a dependency that
  # is already provided never needs to be one of its dependant's inputs.
  childrenOf = parent: map (n: {
    name = n;
    flake = parent.inputs.${n} or (throw
      "standalone deps: ${n} is a dependency but not a flake input of ${parent.outPath}");
  }) (depNamesOf parent);

  walk = queue: seen: acc:
    if queue == [] then acc
    else
      let
        d = builtins.head queue;
        rest = builtins.tail queue;
      in
        if builtins.elem d.name seen then walk rest seen acc
        else walk (rest ++ childrenOf d.flake) (seen ++ [ d.name ]) (acc ++ [ d ]);

  packageNames = map (p: p.name) packages;
  resolved = walk (deps ++ lib.concatMap childrenOf apps) (exclude ++ packageNames) [];

  installOf = d: d.flake.packages.${system}.install or (throw
    "standalone deps: ${d.name} has no packages.${system}.install");
in
pkgs.runCommand "standalone-deps" {
  nativeBuildInputs = lib.optional (packages != []) (
    if lgpm != null then lgpm
    else throw "standalone deps: installing packages needs lgpm");
} ''
  mkdir -p $out/modules $out/plugins
  ${lib.concatMapStrings (p: ''
    found=0
    for lgxFile in ${p.package}/*.lgx; do
      [ -e "$lgxFile" ] || continue
      found=1
      lgpm --modules-dir "$out/modules" --ui-plugins-dir "$out/plugins" install --file "$lgxFile"
    done
    if [ "$found" = 0 ]; then
      echo "standalone deps: no .lgx in ${p.package} for ${p.name}" >&2
      exit 1
    fi
  '') packages}
  ${lib.concatMapStrings (d: ''
    for kind in modules plugins; do
      if [ -d ${installOf d}/$kind ]; then
        cp -rs ${installOf d}/$kind/. $out/$kind/
        chmod -R u+w $out/$kind
      fi
    done
  '') resolved}
''
