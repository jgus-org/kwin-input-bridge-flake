{
  description = "Forward Sunshine's uinput devices into a headless kwin_wayland --virtual session over KWin's EIS RemoteDesktop (libei).";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachSystem [ "x86_64-linux" ] (system:
      let
        pkgs = import nixpkgs { inherit system; };

        kwin-input-bridge = pkgs.stdenv.mkDerivation {
          pname = "kwin-input-bridge";
          version = "0.1";
          src = ./.;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [ pkgs.systemd pkgs.libei ];
          checkInputs = [ pkgs.gtest ];

          buildPhase = ''
            runHook preBuild
            ''${CXX} -O3 -Wall -Wextra -pipe -std=c++23 -o kwin-input-bridge main.cpp kwin-input-bridge.cpp \
              $(pkg-config --cflags --libs libsystemd libei-1.0)
            ''${CXX} -O2 -Wall -Wextra -std=c++23 -I. -o bridge_test \
              test/translation_test.cpp test/eis_test.cpp kwin-input-bridge.cpp \
              $(pkg-config --cflags --libs libsystemd libei-1.0 libeis-1.0 gtest gmock gmock_main)
            runHook postBuild
          '';

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ./bridge_test
            runHook postCheck
          '';

          installPhase = ''
            runHook preInstall
            install -Dm755 kwin-input-bridge "$out/bin/kwin-input-bridge"
            runHook postInstall
          '';

          meta = {
            description = "Forward Sunshine's uinput devices into a headless kwin_wayland --virtual session over KWin's EIS RemoteDesktop";
            mainProgram = "kwin-input-bridge";
            platforms = [ "x86_64-linux" ];
          };
        };
      in
      {
        packages = {
          inherit kwin-input-bridge;
          default = kwin-input-bridge;
        };
      });
}
