{
  description = "Dependency-gate doc-test fixture module";

  inputs.logos-module-builder.url = "github:logos-co/logos-module-builder";

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
