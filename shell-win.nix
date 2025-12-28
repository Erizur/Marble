{ pkgs ? import <nixpkgs> {} }:

pkgs.pkgsCross.mingwW64.mkShell {
  nativeBuildInputs = with pkgs; [
    python3 nodejs which nasm yasm
  ];

  shellHook = ''
    export MOZ_OBJDIR=$(pwd)/obj-win

    # Configure for Windows
    echo "ac_add_options --target=x86_64-w64-mingw32" > .mozconfig
    echo "ac_add_options --enable-application=browser" >> .mozconfig
    echo "ac_add_options --disable-pulseaudio" >> .mozconfig
    echo "ac_add_options --disable-dbus" >> .mozconfig
  '';
}
