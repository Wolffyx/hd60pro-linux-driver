# mz0380.ko for one kernel. Call it from that kernel's package set:
#
#     config.boot.kernelPackages.callPackage ./nix/package.nix { }
#
# This drives kbuild directly instead of the top-level Makefile's `all` and
# `install` targets: those look for the kernel under /lib/modules/$(uname -r),
# which does not exist in a Nix build. The kbuild half of the Makefile (the
# object list and the kernel feature probes) is used unchanged.
{
  lib,
  stdenv,
  kernel,
  # Carries CC/LD for the compiler that built the kernel, so a clang-built
  # kernel gets a clang-built module. Older nixpkgs only has kernel.makeFlags.
  kernelModuleMakeFlags ? kernel.makeFlags,
}:

stdenv.mkDerivation {
  pname = "mz0380";
  # MZ0380_VERSION in src/mz0380.h is the one source for the version.
  version =
    let
      lines = lib.splitString "\n" (builtins.readFile ../src/mz0380.h);
      hit = lib.findFirst (m: m != null) null (
        map (builtins.match ''#define MZ0380_VERSION[[:space:]]+"(.*)"'') lines
      );
    in
    "${lib.head hit}-${kernel.version}";

  # Only what the build reads, so editing docs or scripts rebuilds nothing.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../Makefile
      ../COPYING
      ../src
    ];
  };

  hardeningDisable = [
    "pic"
    "format"
  ];
  nativeBuildInputs = kernel.moduleBuildDependencies;

  makeFlags = kernelModuleMakeFlags ++ [
    "-C"
    "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "M=$(PWD)"
    "INSTALL_MOD_PATH=$(out)"
  ];
  buildFlags = [ "modules" ];
  installTargets = [ "modules_install" ];

  meta = {
    description = "Linux driver for the Elgato Game Capture HD60 Pro (YUAN MZ0380)";
    homepage = "https://github.com/Wolffyx/hd60pro-linux-driver";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
  };
}
