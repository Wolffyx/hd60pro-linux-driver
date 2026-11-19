# NixOS module: builds mz0380.ko for whatever kernel the system uses
# (boot.kernelPackages) and adds it to the module tree. The driver then loads
# by itself when the card is present.
{ config, lib, ... }:

let
  cfg = config.hardware.mz0380;
  # The card only works behind a translating IOMMU - see README "Requirements".
  badIommu = lib.filter (p: p == "iommu=pt" || p == "iommu=off") config.boot.kernelParams;
in
{
  options.hardware.mz0380.enable =
    lib.mkEnableOption "the mz0380 driver for the Elgato Game Capture HD60 Pro";

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [
      (config.boot.kernelPackages.callPackage ./package.nix { })
    ];

    # Mainline (and the NixOS kernel) leaves CONFIG_INTEL_IOMMU_DEFAULT_ON off,
    # so on Intel VT-d stays disabled without this and the driver finds no
    # IOMMU domain. Ignored on AMD, where AMD-Vi is on by default.
    boot.kernelParams = [ "intel_iommu=on" ];

    warnings =lib.optional (badIommu != [ ]) ''
      hardware.mz0380: boot.kernelParams contains ${lib.concatStringsSep " " badIommu}.
      The HD60 Pro needs a translating IOMMU; with this setting the video node
      appears but streaming is refused. See "Keeping iommu=pt" in the driver's
      README for iommu=pt; iommu=off cannot work.
    '';
  };
}
