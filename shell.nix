{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    rustc cargo clang llvmPackages.bintools
    nodejs python3 gnum4 pkg-config which
    nasm yasm
    gtk3 dbus glib xorg.libXt
  ];

  buildInputs = with pkgs; [
    alsa-lib
    libpulseaudio
    xorg.libX11
  ];

  CC = "clang";
  CXX = "clang++";

  shellHook = ''
    export MOZ_OBJDIR=$(pwd)/obj-linux

    # Create a mozconfig that forces bundling
    echo "ac_add_options --enable-application=browser" > .mozconfig
    echo "ac_add_options --enable-optimize" >> .mozconfig
    # Explicitly DISABLE system libs to be safe
    echo "ac_add_options --without-system-icu" >> .mozconfig
    echo "ac_add_options --without-system-nspr" >> .mozconfig
    echo "ac_add_options --without-system-nss" >> .mozconfig
    echo "ac_add_options --without-system-zlib" >> .mozconfig
  '';
}
