{
  description = "Forward Sunshine's uinput devices into a headless kwin_wayland --virtual session over KWin's EIS RemoteDesktop (libei).";

  inputs = {
    flake-lib.url = "github:jgus-org/flake-lib/v1";
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    {
      self,
      flake-lib,
      nixpkgs,
    }:
    flake-lib.lib.eachDefaultLinuxSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        orgFormatting = flake-lib.lib.mkFormatting {
          inherit self system;
        };

        kwin-input-bridge = pkgs.stdenv.mkDerivation {
          buildInputs = [
            pkgs.systemd
            pkgs.libei
          ];
          buildPhase = ''
            runHook preBuild
            ''${CXX} -O3 -Wall -Wextra -pipe -std=c++23 -o kwin-input-bridge main.cpp kwin-input-bridge.cpp \
              $(pkg-config --cflags --libs libsystemd libei-1.0)
            ''${CXX} -O2 -Wall -Wextra -std=c++23 -I. -o bridge_test \
              test/translation_test.cpp test/eis_test.cpp kwin-input-bridge.cpp \
              $(pkg-config --cflags --libs libsystemd libei-1.0 libeis-1.0 gtest gmock gmock_main)
            runHook postBuild
          '';
          checkInputs = [ pkgs.gtest ];
          checkPhase = ''
            runHook preCheck
            ./bridge_test
            runHook postCheck
          '';
          doCheck = true;
          installPhase = ''
            runHook preInstall
            install -Dm755 kwin-input-bridge "$out/bin/kwin-input-bridge"
            runHook postInstall
          '';
          meta = {
            description = "Forward Sunshine's uinput devices into a headless kwin_wayland --virtual session over KWin's EIS RemoteDesktop";
            mainProgram = "kwin-input-bridge";
            platforms = flake-lib.lib.defaultLinuxSystems;
          };
          nativeBuildInputs = [ pkgs.pkg-config ];
          pname = "kwin-input-bridge";
          src = ./.;
          version = "0.1";
        };
      in
      {
        checks.formatting = orgFormatting.check;
        formatter = orgFormatting.formatter;
        packages = {
          inherit kwin-input-bridge;
          default = kwin-input-bridge;
        };
      }
    );
}
