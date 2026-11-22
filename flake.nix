{
  description = "C and C++ development shells with Bison, Flex, and Wasmtime";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { nixpkgs, ... }: {
    devShells = nixpkgs.lib.genAttrs [ "aarch64-linux" "x86_64-linux" ] (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        muslPkgs = {
          aarch64-linux = pkgs.pkgsCross.aarch64-multiplatform-musl;
          x86_64-linux = pkgs.pkgsCross.musl64;
        }.${system};
        mkShell = targetPkgs:
          (targetPkgs.mkShell.override { stdenv = targetPkgs.gccStdenv; }) {
            nativeBuildInputs = with pkgs; [ bison flex python3 wasmtime ];
            buildInputs = [ targetPkgs.gtest ];
          };
      in {
        default = mkShell pkgs;
        musl = (mkShell muslPkgs).overrideAttrs (_: {
          MCC_TOOLCHAIN = "${muslPkgs.gccStdenv.cc}/bin/${muslPkgs.gccStdenv.cc.targetPrefix}gcc";
          MCC_GCC_HOST = "${pkgs.gccStdenv.cc}/bin/gcc";
          MCC_COMPILER_INCLUDE = "${./include}";
          MCC_SYSTEM_INCLUDE = "${nixpkgs.lib.getDev muslPkgs.musl}/include";
        });
      });
  };
}
