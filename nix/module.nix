# NixOS module: builds mz0380.ko for whatever kernel the system uses
# (boot.kernelPackages) and adds it to the module tree. The driver then loads
# by itself when the card is present.
{ config, lib, ... }:

let
  cfg = config.hardware.mz0380;
  # The card only works behind a translating IOMMU - see README "Requirements".
  badIommu = lib.filter (p: p == "iommu=pt" || p == "iommu=off") config.boot.kernelParams;

  # Only what the user set: the driver's built-in defaults are the validated
  # configuration, so nothing is written for an option left alone.
  params = lib.optionalAttrs cfg.enableAudio { enable_audio = true; } // cfg.moduleOptions;
  render = v: if lib.isBool v then (if v then "1" else "0") else toString v;
  optionsLine = lib.concatStringsSep " " (lib.mapAttrsToList (k: v: "${k}=${render v}") params);
in
{
  options.hardware.mz0380 = {
    enable = lib.mkEnableOption "the mz0380 driver for the Elgato Game Capture HD60 Pro";

    enableAudio = lib.mkEnableOption ''
      the card's HDMI audio as an ALSA capture device (module parameter
      enable_audio, experimental). Takes effect when the module is next loaded,
      i.e. after a reboot or `modprobe -r mz0380 && modprobe mz0380`
    '';

    moduleOptions = lib.mkOption {
      type = with lib.types; attrsOf (oneOf [ bool int str ]);
      default = { };
      example = {
        raw_deliver = false;
        card = "2";
      };
      description = ''
        Other mz0380 module parameters, written to the generated modprobe
        configuration as `options mz0380 name=value`. `modinfo mz0380` lists
        them. Booleans are written as 1/0. A key set here wins over
        `enableAudio`.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [
      (config.boot.kernelPackages.callPackage ./package.nix { })
    ];

    boot.extraModprobeConfig = lib.mkIf (params != { }) ''
      options mz0380 ${optionsLine}
    '';

    # Mainline (and the NixOS kernel) leaves CONFIG_INTEL_IOMMU_DEFAULT_ON off,
    # so on Intel VT-d stays disabled without this and the driver finds no
    # IOMMU domain. Ignored on AMD, where AMD-Vi is on by default.
    boot.kernelParams = [ "intel_iommu=on" ];

    warnings = lib.optional (badIommu != [ ]) ''
      hardware.mz0380: boot.kernelParams contains ${lib.concatStringsSep " " badIommu}.
      The HD60 Pro needs a translating IOMMU; with this setting the video node
      appears but streaming is refused. See "Keeping iommu=pt" in the driver's
      README for iommu=pt; iommu=off cannot work.
    '';
  };
}
