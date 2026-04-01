{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
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
}
