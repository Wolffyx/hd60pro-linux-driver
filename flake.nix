{
  description = "Linux driver for the Elgato Game Capture HD60 Pro (YUAN MZ0380)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      # The card is a PCIe board and has only been run on x86_64.
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      # What a NixOS system uses: builds against that system's own kernel.
      nixosModules.default = import ./nix/module.nix;

      # `nix build` smoke test against nixpkgs' default kernel. Not for
      # installing - a module only loads into the kernel it was built for.
      packages.${system}.default = pkgs.linuxPackages.callPackage ./nix/package.nix { };
    };
}
