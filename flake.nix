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
          # Keep libc.so ahead of the separate Nix output containing libc.a.
          NIX_LDFLAGS = "-L${pkgs.glibc}/lib -L${pkgs.glibc.static}/lib";
        };
      });
  };
}
