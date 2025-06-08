let
  pkgs = import <nixpkgs> {};  # or use an overlay/flake as needed
in
pkgs.callPackage ./marble.nix {}
