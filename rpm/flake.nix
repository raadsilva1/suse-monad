{
  description = "NixOS dev shell and builder for suse-monad RPM";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          bash
          coreutils
          findutils
          gawk
          gcc
          gnumake
          gnugrep
          gnutar
          gzip
          rpm
        ];
      };

      apps.${system}.build-rpm = {
        type = "app";
        program = "${pkgs.writeShellScript "suse-monad-build-rpm" ''
          set -euo pipefail
          exec ${pkgs.bash}/bin/bash ${./build-rpm-nixos.sh} "$@"
        ''}";
      };
    };
}
