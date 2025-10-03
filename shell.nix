{ lib
, pkgs
, stdenv
, fetchpatch
, patchelf

# build time
, autoconf
, cargo
, dump_syms
, makeWrapper
, mimalloc
, nodejs
, perl
, pkg-config
, pkgsCross # wasm32 rlbox
, python3
, runCommand
, rustc
, rust-cbindgen
, rustPlatform
, unzip
, which
, wrapGAppsHook

# runtime
, bzip2
, dbus
, dbus-glib
, fontconfig
, freetype
, glib
, gnum4
, gtk3
, icu73
, libGL
, libGLU
, libevent
, libffi
, libjpeg
, libpng
, libstartup_notification
, libvpx
, libwebp
, nasm
, nspr
, nss_esr
, nss_latest
, pango
, xorg
, zip
, zlib
, pkgsBuildBuild

# optionals

## debugging

, debugBuild ? false

# On 32bit platforms, we disable adding "-g" for easier linking.
, enableDebugSymbols ? !stdenv.is32bit

## optional libraries

, alsaSupport ? stdenv.isLinux, alsa-lib
, ffmpegSupport ? true
, gssSupport ? true, libkrb5
, jackSupport ? stdenv.isLinux, libjack2
, jemallocSupport ? !stdenv.hostPlatform.isMusl, jemalloc
, ltoSupport ? (stdenv.isLinux && stdenv.is64bit && !stdenv.hostPlatform.isRiscV), overrideCC, buildPackages
, pgoSupport ? (stdenv.isLinux && stdenv.hostPlatform == stdenv.buildPlatform), xvfb-run
, pipewireSupport ? waylandSupport && webrtcSupport
, pulseaudioSupport ? stdenv.isLinux, libpulseaudio
, sndioSupport ? stdenv.isLinux, sndio
, waylandSupport ? true, libxkbcommon, libdrm

## privacy-related options

, privacySupport ? false

# WARNING: NEVER set any of the options below to `true` by default.
# Set to `!privacySupport` or `false`.

, crashreporterSupport ? !privacySupport && !stdenv.hostPlatform.isRiscV && !stdenv.hostPlatform.isMusl, curl
, geolocationSupport ? !privacySupport
, googleAPISupport ? geolocationSupport
, mlsAPISupport ? geolocationSupport
, webrtcSupport ? !privacySupport && !stdenv.hostPlatform.isRiscV

# digital rights management

# This flag controls whether Firefox will show the nagbar, that allows
# users at runtime the choice to enable Widevine CDM support when a site
# requests it.
# Controlling the nagbar and widevine CDM at runtime is possible by setting
# `browser.eme.ui.enabled` and `media.gmp-widevinecdm.enabled` accordingly
, drmSupport ? true
, enableOfficialBranding ? true
}:

assert stdenv.cc.libc or null != null;
assert pipewireSupport -> !waylandSupport || !webrtcSupport -> throw "Marble: pipewireSupport requires both wayland and webrtc support.";

let
  inherit (lib) enableFeature;

  # Target the LLVM version that rustc is built with for LTO.
  llvmPackages0 = rustc.llvmPackages;
  llvmPackagesBuildBuild0 = pkgsBuildBuild.rustc.llvmPackages;

  # Force the use of lld and other llvm tools for LTO
  llvmPackages = llvmPackages0.override {
    bootBintoolsNoLibc = null;
    bootBintools = null;
  };
  llvmPackagesBuildBuild = llvmPackagesBuildBuild0.override {
    bootBintoolsNoLibc = null;
    bootBintools = null;
  };

  # LTO requires LLVM bintools including ld.lld and llvm-ar.
  buildStdenv = overrideCC llvmPackages.stdenv (llvmPackages.stdenv.cc.override {
    bintools = if ltoSupport then buildPackages.rustc.llvmPackages.bintools else stdenv.cc.bintools;
  });

  # Compile the wasm32 sysroot to build the RLBox Sandbox
  # https://hacks.mozilla.org/2021/12/webassembly-and-back-again-fine-grained-sandboxing-in-firefox-95/
  # We only link c++ libs here, our compiler wrapper can find wasi libc and crt itself.
  wasiSysRoot = runCommand "wasi-sysroot" {} ''
    mkdir -p $out/lib/wasm32-wasi
    for lib in ${pkgsCross.wasi32.llvmPackages.libcxx}/lib/*; do
      ln -s $lib $out/lib/wasm32-wasi
    done
  '';

in

buildStdenv.mkDerivation {
  pname = "marble-unwrapped";
  version = "G1.1";

  src = ./xpcom/ds/tools/.;

  meta = with pkgs.lib; {
    description = "Marble Browser - development";
    homepage = "https://github.com/NetworkNeighborhood/Marble";
    license = licenses.mpl20;
  };

  outputs = [
    "out"
  ]
  ++ lib.optionals crashreporterSupport [ "symbols" ];

  # Add another configure-build-profiling run before the final configure phase if we build with pgo
  preConfigurePhases = lib.optionals pgoSupport [
    "configurePhase"
    "buildPhase"
    "profilingPhase"
  ];

  postPatch = ''
    rm -rf obj-x86_64-pc-linux-gnu
    patchShebangs mach build
  '';

  # Ignore trivial whitespace changes in patches, this fixes compatibility of
  # ./env_var_for_system_dir.patch with Firefox >=65 without having to track
  # two patches.
  patchFlags = [ "-p1" "-l" ];

  # if not explicitly set, wrong cc from buildStdenv would be used
  HOST_CC = "${llvmPackagesBuildBuild.stdenv.cc}/bin/cc";
  HOST_CXX = "${llvmPackagesBuildBuild.stdenv.cc}/bin/c++";

  nativeBuildInputs = [
    autoconf
    cargo
    gnum4
    llvmPackagesBuildBuild.bintools
    makeWrapper
    nodejs
    perl
    pkg-config
    python3
    rust-cbindgen
    rustPlatform.bindgenHook
    rustc
    unzip
    which
    wrapGAppsHook
  ]
  ++ lib.optionals crashreporterSupport [ dump_syms patchelf ]
  ++ lib.optionals pgoSupport [ xvfb-run ];

  setOutputFlags = false; # `./mach configure` doesn't understand `--*dir=` flags.

  configureFlags = [
    "--disable-tests"
    "--disable-updater"
    "--enable-application=browser"
    "--enable-default-toolkit=cairo-gtk3${lib.optionalString waylandSupport "-wayland"}"
    "--enable-system-pixman"
    "--with-libclang-path=${llvmPackagesBuildBuild.libclang.lib}/lib"
    "--with-system-ffi"
    "--with-system-icu"
    "--with-system-jpeg"
    "--with-system-libevent"
    "--with-system-libvpx"
    "--with-system-nspr"
    "--with-system-nss"
    "--with-system-png" # needs APNG support
    "--with-system-webp"
    "--with-system-zlib"
    "--with-wasi-sysroot=${wasiSysRoot}"
    # for firefox, host is buildPlatform, target is hostPlatform
    "--host=${buildStdenv.buildPlatform.config}"
    "--target=${buildStdenv.hostPlatform.config}"
  ]
  # LTO is done using clang and lld on Linux.
  ++ lib.optionals ltoSupport [
     "--enable-lto=cross" # Cross-Language LTO
     "--enable-linker=lld"
  ]
  # elf-hack is broken when using clang+lld:
  # https://bugzilla.mozilla.org/show_bug.cgi?id=1482204
  ++ lib.optional (ltoSupport && (buildStdenv.isAarch32 || buildStdenv.isi686 || buildStdenv.isx86_64)) "--disable-elf-hack"
  ++ lib.optional (!drmSupport) "--disable-eme"
  ++ [
    (enableFeature alsaSupport "alsa")
    (enableFeature crashreporterSupport "crashreporter")
    (enableFeature ffmpegSupport "ffmpeg")
    (enableFeature geolocationSupport "necko-wifi")
    (enableFeature gssSupport "negotiateauth")
    (enableFeature jackSupport "jack")
    (enableFeature jemallocSupport "jemalloc")
    (enableFeature pulseaudioSupport "pulseaudio")
    (enableFeature sndioSupport "sndio")
    (enableFeature webrtcSupport "webrtc")
    (enableFeature debugBuild "debug")
    (if debugBuild then "--enable-profiling" else "--enable-optimize")
    # --enable-release adds -ffunction-sections & LTO that require a big amount
    # of RAM, and the 32-bit memory space cannot handle that linking
    (enableFeature (!debugBuild && !stdenv.is32bit) "release")
    (enableFeature enableDebugSymbols "debug-symbols")
  ]
  ++ lib.optionals enableDebugSymbols [ "--disable-strip" "--disable-install-strip" ]
  ++ lib.optional enableOfficialBranding "--enable-official-branding";

  shellHook = ''
    # Runs autoconf through ./mach configure in configurePhase
    configureScript="$(realpath ./mach) configure"

    # Set predictable directories for build and state
    export MOZ_OBJDIR=$(pwd)/mozobj
    export MOZBUILD_STATE_PATH=$(pwd)/mozbuild

    # Set consistent remoting name to ensure wmclass matches with desktop file
    export MOZ_APP_REMOTINGNAME="marble-browser"

    # AS=as in the environment causes build failure
    # https://bugzilla.mozilla.org/show_bug.cgi?id=1497286
    unset AS

    # Use our own python
    export MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=system

    # RBox WASM Sandboxing
    export WASM_CC=${pkgsCross.wasi32.stdenv.cc}/bin/${pkgsCross.wasi32.stdenv.cc.targetPrefix}cc
    export WASM_CXX=${pkgsCross.wasi32.stdenv.cc}/bin/${pkgsCross.wasi32.stdenv.cc.targetPrefix}c++

    sed -i "s/icu-i18n/icu-uc &/" js/moz.configure
  '' + lib.optionalString pgoSupport ''
    if [ -e "$TMPDIR/merged.profdata" ]; then
      echo "Configuring with profiling data"
      for i in "''${!configureFlagsArray[@]}"; do
        if [[ ''${configureFlagsArray[i]} = "--enable-profile-generate=cross" ]]; then
          unset 'configureFlagsArray[i]'
        fi
      done
      configureFlagsArray+=(
        "--enable-profile-use=cross"
        "--with-pgo-profile-path="$TMPDIR/merged.profdata""
        "--with-pgo-jarlog="$TMPDIR/jarlog""
      )
      ${lib.optionalString stdenv.hostPlatform.isMusl ''
        LDFLAGS="$OLD_LDFLAGS"
        unset OLD_LDFLAGS
      ''}
    else
      echo "Configuring to generate profiling data"
      configureFlagsArray+=(
        "--enable-profile-generate=cross"
      )
      ${lib.optionalString stdenv.hostPlatform.isMusl
      # Set the rpath appropriately for the profiling run
      # During the profiling run, loading libraries from $out would fail,
      # since the profiling build has not been installed to $out
      ''
        OLD_LDFLAGS="$LDFLAGS"
        LDFLAGS="-Wl,-rpath,$(pwd)/mozobj/dist/marble"
      ''}
    fi
  '' + lib.optionalString (enableOfficialBranding && !stdenv.is32bit) ''
    export MOZILLA_OFFICIAL=1

    export LIBCLANG_PATH=${llvmPackagesBuildBuild.libclang.lib}/lib
    export WASI_SYSROOT=${wasiSysRoot}
  '' + lib.optionalString stdenv.hostPlatform.isMusl ''
    # linking firefox hits the vm.max_map_count kernel limit with the default musl allocator
    # TODO: Default vm.max_map_count has been increased, retest without this
    export LD_PRELOAD=${mimalloc}/lib/libmimalloc.so
  '';

  buildInputs = with pkgs; [
    bzip2
    dbus
    dbus-glib
    file
    fontconfig
    freetype
    glib
    gtk3
    libffi
    libGL
    libGLU
    libevent
    libjpeg
    libpng
    libstartup_notification
    libvpx
    libwebp
    nasm
    nspr
    pango
    perl
    xorg.libX11
    xorg.libXcursor
    xorg.libXdamage
    xorg.libXext
    xorg.libXft
    xorg.libXi
    xorg.libXrender
    xorg.libXt
    xorg.libXtst
    xorg.pixman
    xorg.xorgproto
    zip
    zlib
    icu73
    nss_latest
  ]
  ++ lib.optional  alsaSupport alsa-lib
  ++ lib.optional  jackSupport libjack2
  ++ lib.optional  pulseaudioSupport libpulseaudio # only headers are needed
  ++ lib.optional  sndioSupport sndio
  ++ lib.optional  gssSupport libkrb5
  ++ lib.optionals waylandSupport [ libxkbcommon libdrm ]
  ++ lib.optional  jemallocSupport jemalloc;

  separateDebugInfo = enableDebugSymbols;
  enableParallelBuilding = true;
  env = lib.optionalAttrs stdenv.hostPlatform.isMusl {
    # Firefox relies on nonstandard behavior of the glibc dynamic linker. It re-uses
    # previously loaded libraries even though they are not in the rpath of the newly loaded binary.
    # On musl we have to explicity set the rpath to include these libraries.
    LDFLAGS = "-Wl,-rpath,${placeholder "out"}/lib/marble";
  };

  # tests were disabled in configureFlags
  doCheck = false;

  passthru = {
    inherit alsaSupport;
    inherit jackSupport;
    inherit pipewireSupport;
    inherit sndioSupport;
    inherit nspr;
    inherit ffmpegSupport;
    inherit gssSupport;
    inherit gtk3;
    inherit wasiSysRoot;
    version = "G1.1";
  };

  hardeningDisable = [ "format" ]; # -Werror=format-security
  dontFixLibtool = true;

  # on aarch64 this is also required
  dontUpdateAutotoolsGnuConfigScripts = true;

  requiredSystemFeatures = [ "big-parallel" ];
}
