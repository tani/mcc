{
  description = "Development shell for mcc";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { nixpkgs, ... }: {
    devShells = nixpkgs.lib.genAttrs [ "aarch64-linux" "x86_64-linux" ] (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        default = (pkgs.mkShell.override { stdenv = pkgs.gccStdenv; }) {
          nativeBuildInputs = with pkgs; [ bison flex python3 wasmtime ];
          buildInputs = [ pkgs.gtest ];
          MCC_SYSTEM_INCLUDE = "${nixpkgs.lib.getDev pkgs.glibc}/include";
          MCC_SYSTEM_LIB = "${pkgs.glibc}/lib";
          MCC_STATIC_LIB = "${pkgs.glibc.static}/lib";
          MCC_RUNTIME_LIB = "${pkgs.gcc.cc}/lib/gcc/${pkgs.stdenv.hostPlatform.config}/${pkgs.gcc.cc.version}";
          NIX_LDFLAGS = "-L${pkgs.glibc}/lib -L${pkgs.glibc.static}/lib";
        };
      });
  };
}
