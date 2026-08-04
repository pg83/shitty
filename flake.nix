# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.
{
  description = "Shitty — a small, fast Wayland/Vulkan terminal emulator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    {
      self,
      nixpkgs,
    }:
    let
      inherit (nixpkgs) lib;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = lib.genAttrs systems;

      nixpkgsFor = system: nixpkgs.legacyPackages.${system};

      # YYYY.MM.DD from the flake's lastModifiedDate, matching build.py's scheme.
      versionFromFlake =
        let
          d = self.lastModifiedDate or "19700101";
        in
        "${builtins.substring 0 4 d}.${builtins.substring 4 2 d}.${builtins.substring 6 2 d}";

      sanitizerConfigs = {
        asan = {
          flag = "-fsanitize=address";
          environment = "export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1";
        };
        ubsan = {
          flag = "-fsanitize=undefined";
          environment = "export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1";
        };
      };

      configureBuildEnvironment =
        {
          sanitizer ? null,
          coverage ? false,
        }:
        assert !(coverage && sanitizer != null);
        let
          config = if sanitizer == null then null else sanitizerConfigs.${sanitizer};
          cxxFlags =
            if config == null then
              ""
            else
              lib.concatStringsSep " " [
                config.flag
                "-fno-sanitize-recover=all"
                "-fno-omit-frame-pointer"
                "-g"
              ];
          linkInstrumentation =
            if config != null then
              config.flag
            else if coverage then
              "-fprofile-instr-generate -Wl,--build-id=sha1"
            else
              "";
        in
        ''
          unset CPPFLAGS CFLAGS CXXFLAGS LDFLAGS ASAN_OPTIONS UBSAN_OPTIONS LLVM_PROFILE_FILE
          # build intentionally passes its canonical target triple. Nix's
          # wrapper spells the equivalent native vendor field differently.
          export NIX_CC_WRAPPER_SUPPRESS_TARGET_WARNING=1
          ${lib.optionalString (config != null) ''
            export CXXFLAGS=${lib.escapeShellArg cxxFlags}
            ${config.environment}
          ''}
          ${lib.optionalString coverage ''
            export CXXFLAGS="-fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. -fcoverage-prefix-map=$PWD=."
          ''}
          export LDFLAGS="${
            lib.optionalString (linkInstrumentation != "") "${linkInstrumentation} "
          }$(pkg-config --libs wayland-client xkbcommon) -lrt"
        '';

      mkShitty =
        pkgs:
        {
          sanitizer ? null,
        }:
        let
          stdenv = pkgs.llvmPackages.stdenv;
          sanitizerSuffix = lib.optionalString (sanitizer != null) "-${sanitizer}";
          buildDirectory = ".build${sanitizerSuffix}";
        in
        stdenv.mkDerivation rec {
          pname = "shitty${sanitizerSuffix}";
          version = versionFromFlake;

          src = self;

          # build.py stamps SHITTY_VERSION from date.today(), which is impure
          # under the Nix sandbox. Pin it to the flake revision date instead.
          postPatch = ''
            substituteInPlace build.py \
              --replace-fail 'date.today().strftime("%Y.%m.%d")' '"${version}"'
          '';

          nativeBuildInputs = with pkgs; [
            addDriverRunpath
            glslang
            librsvg
            makeWrapper
            pkg-config
            python3
            ragel
            wayland-protocols
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            brotli
            fontconfig
            freetype
            harfbuzz
            libxkbcommon
            simdutf
            utf8proc
            vulkan-headers
            vulkan-loader
            wayland
          ];

          # The project's runner honours the usual toolchain env vars. Keep the
          # build out of $src (read-only store path) via -B.
          buildPhase = ''
            runHook preBuild
            ${configureBuildEnvironment { inherit sanitizer; }}
            python3 ./build -B ${buildDirectory} -j "$NIX_BUILD_CORES"
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            install -Dm755 ${buildDirectory}/st "$out/bin/st"
            install -Dm644 shitty.desktop \
              "$out/share/applications/shitty.desktop"
            install -Dm644 shitty.svg \
              "$out/share/icons/hicolor/scalable/apps/shitty.svg"
            runHook postInstall
          '';

          # Vulkan ICDs live under /run/opengl-driver on NixOS; addDriverRunpath
          # puts that directory on the binary's RUNPATH.
          postFixup = ''
            addDriverRunpath "$out/bin/st"
          '';

          meta = {
            description = "Small, fast terminal emulator with a Linux Wayland/Vulkan frontend";
            homepage = "https://github.com/pg83/shitty";
            license = with lib.licenses; [
              gpl3Plus
              mit
            ];
            mainProgram = "st";
            platforms = lib.platforms.linux;
            # Wayland + Vulkan compute frontend; no X11 backend.
            badPlatforms = lib.platforms.darwin;
          };
        };

      mkTestCheck =
        pkgs:
        {
          sanitizer ? null,
          coverage ? false,
        }:
        assert !(coverage && sanitizer != null);
        let
          base = mkShitty pkgs { inherit sanitizer; };
          sanitizerSuffix = lib.optionalString (sanitizer != null) "-${sanitizer}";
          checkSuffix = if coverage then "-coverage" else sanitizerSuffix;
          buildDirectory = ".build-tests${checkSuffix}";
        in
        base.overrideAttrs (old: {
          pname = "shitty-tests${checkSuffix}";

          nativeBuildInputs =
            old.nativeBuildInputs
            ++ [
              pkgs.ncurses
              pkgs.perl
              pkgs.vttest
            ]
            ++ lib.optionals (sanitizer != null || coverage) [ pkgs.llvmPackages.llvm ]
            ++ lib.optionals coverage [ pkgs.lcov ];

          postPatch = old.postPatch + ''
            # Some vendored xterm scripts invoke other scripts by their
            # /usr/bin/env shebang. Rewrite those interpreters for the Nix
            # sandbox without modifying the upstream fixtures in git.
            patchShebangs \
              tests/xterm_vttests/upstream \
              tests/xterm_vttests/bin
            for script in $(grep -l "CMD='/bin/echo'" tests/xterm_vttests/upstream/*.sh); do
              # The prefix adapter preserves and rewrites these two unbounded
              # generators in memory; leave their upstream source intact.
              case "$script" in
                */8colors.sh|*/16colors.sh) continue ;;
              esac
              substituteInPlace "$script" \
                --replace-fail "CMD='/bin/echo'" "CMD='echo'"
            done
          '';

          FONTCONFIG_FILE = pkgs.makeFontsConf {
            fontDirectories = with pkgs; [
              dejavu_fonts
              ibm-plex
            ];
          };

          buildPhase = ''
            runHook preBuild
            ${configureBuildEnvironment { inherit sanitizer coverage; }}
            ${lib.optionalString (sanitizer == "asan") ''
              export ASAN_SYMBOLIZER_PATH=${lib.getExe' pkgs.llvmPackages.llvm "llvm-symbolizer"}
            ''}
            ${lib.optionalString coverage ''
              profileDirectory="$TMPDIR/shitty-coverage-profiles"
              mkdir -p "$profileDirectory"
              export LLVM_PROFILE_FILE="$profileDirectory/%b-%16m.profraw"
            ''}
            python3 ./build \
              -B ${buildDirectory} \
              -j "$NIX_BUILD_CORES" \
              -k \
              test
            ${lib.optionalString coverage ''
              # Groups deliberately do not publish their outputs. Ask the
              # runner for the three coverage binaries explicitly so their
              # stable source-root symlinks can be passed to llvm-cov.
              python3 ./build \
                -B ${buildDirectory} \
                -j "$NIX_BUILD_CORES" \
                st_test unit_tests plt_wayland_integration_tests
              coverageDirectory="$PWD/.coverage"
              coverageIgnore='(^|/)(tests|third_party/libstd|\.build[^/]*)/|(^|/)[^/]*_ut\.cpp$|(^|/)(test_mode|test_input)\.(cpp|h)$|^/nix/store/'
              mkdir -p "$coverageDirectory"
              coverageProfiles=()
              for binary in ./st_test ./unit_tests; do
                buildId="$(llvm-readelf -n "$binary" |
                  sed -n 's/.*Build ID: //p' |
                  head -1)"
                if [[ -z "$buildId" ]]; then
                  echo "coverage binary has no build ID: $binary" >&2
                  exit 1
                fi
                binaryProfiles=("$profileDirectory/$buildId"-*.profraw)
                if [[ ! -e "''${binaryProfiles[0]}" ]]; then
                  echo "coverage binary produced no profiles: $binary" >&2
                  exit 1
                fi
                coverageProfiles+=("''${binaryProfiles[@]}")
              done
              waylandBinary=./plt_wayland_integration_tests
              waylandBuildId="$(llvm-readelf -n "$waylandBinary" |
                sed -n 's/.*Build ID: //p' |
                head -1)"
              if [[ -z "$waylandBuildId" ]]; then
                echo "coverage binary has no build ID: $waylandBinary" >&2
                exit 1
              fi
              waylandProfiles=("$profileDirectory/$waylandBuildId"-*.profraw)
              if [[ ! -e "''${waylandProfiles[0]}" ]]; then
                echo "coverage binary produced no profiles: $waylandBinary" >&2
                exit 1
              fi
              llvm-profdata merge \
                -sparse \
                "''${coverageProfiles[@]}" \
                -o "$coverageDirectory/coverage.profdata"
              llvm-profdata merge \
                -sparse \
                "''${waylandProfiles[@]}" \
                -o "$coverageDirectory/wayland.profdata"
              llvm-cov export \
                ./st_test \
                -object=./unit_tests \
                -instr-profile="$coverageDirectory/coverage.profdata" \
                -format=lcov \
                -ignore-filename-regex="$coverageIgnore" \
                > "$coverageDirectory/core.info"
              llvm-cov export \
                "$waylandBinary" \
                -instr-profile="$coverageDirectory/wayland.profdata" \
                -format=lcov \
                -ignore-filename-regex="$coverageIgnore" \
                > "$coverageDirectory/wayland.info"
              lcov \
                --branch-coverage \
                --add-tracefile "$coverageDirectory/core.info" \
                --add-tracefile "$coverageDirectory/wayland.info" \
                --output-file "$coverageDirectory/coverage.info"
              {
                echo "Core coverage"
                llvm-cov report \
                  ./st_test \
                  -object=./unit_tests \
                  -instr-profile="$coverageDirectory/coverage.profdata" \
                  -ignore-filename-regex="$coverageIgnore"
                echo
                echo "Wayland integration coverage"
                llvm-cov report \
                  "$waylandBinary" \
                  -instr-profile="$coverageDirectory/wayland.profdata" \
                  -ignore-filename-regex="$coverageIgnore"
              } > "$coverageDirectory/summary.txt"
              llvm-cov show \
                ./st_test \
                -object=./unit_tests \
                -instr-profile="$coverageDirectory/coverage.profdata" \
                -format=html \
                -output-dir="$coverageDirectory/html" \
                -show-branches=percent \
                -coverage-watermark=80,50 \
                -ignore-filename-regex="$coverageIgnore"
              llvm-cov show \
                "$waylandBinary" \
                -instr-profile="$coverageDirectory/wayland.profdata" \
                -format=html \
                -output-dir="$coverageDirectory/html/wayland" \
                -show-branches=percent \
                -coverage-watermark=80,50 \
                -ignore-filename-regex="$coverageIgnore"
              substituteInPlace "$coverageDirectory/coverage.info" \
                --replace-quiet "SF:$PWD/" "SF:"
              if grep -q '^SF:/' "$coverageDirectory/coverage.info"; then
                echo "coverage report contains absolute source paths" >&2
                grep '^SF:/' "$coverageDirectory/coverage.info" | head -10 >&2
                exit 1
              fi
              if ! grep -q '^SF:' "$coverageDirectory/coverage.info"; then
                echo "coverage report does not contain source files" >&2
                exit 1
              fi
              cat "$coverageDirectory/summary.txt"
            ''}
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p "$out"
            ${
              if coverage then
                ''
                  install -Dm644 .coverage/coverage.info "$out/coverage.info"
                  install -Dm644 .coverage/summary.txt "$out/summary.txt"
                  cp -R .coverage/html "$out/html"
                ''
              else
                ''
                  touch "$out/passed"
                ''
            }
            runHook postInstall
          '';

          postFixup = "";
        });

      mkDevShell =
        pkgs:
        let
          stdenv = pkgs.llvmPackages.stdenv;
          shitty = mkShitty pkgs { };
        in
        pkgs.mkShell.override { inherit stdenv; } {
          inputsFrom = [ shitty ];
          NIX_CC_WRAPPER_SUPPRESS_TARGET_WARNING = "1";

          packages = with pkgs; [
            clang-tools
            gdb
            ncurses
            perl
            vttest
            # Fonts for manual runs inside the shell.
            dejavu_fonts
            ibm-plex
          ];

          FONTCONFIG_FILE = pkgs.makeFontsConf {
            fontDirectories = with pkgs; [
              dejavu_fonts
              ibm-plex
            ];
          };

          shellHook = ''
            # Keep ambient toolchain flags from contaminating this shell.
            unset CPPFLAGS CFLAGS CXXFLAGS LDFLAGS
            export LDFLAGS="$(pkg-config --libs wayland-client xkbcommon) -lrt"
            echo "shitty dev shell — run: ./build && ./st"
          '';
        };
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgsFor system;
          shitty = mkShitty pkgs { };
        in
        {
          default = shitty;
          shitty = shitty;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgsFor system;
        in
        {
          default = mkDevShell pkgs;
        }
      );

      checks = forAllSystems (
        system:
        let
          pkgs = nixpkgsFor system;
        in
        {
          build = mkShitty pkgs { };
          tests = mkTestCheck pkgs { };
          coverage = mkTestCheck pkgs { coverage = true; };
          build-asan = mkShitty pkgs { sanitizer = "asan"; };
          tests-asan = mkTestCheck pkgs { sanitizer = "asan"; };
          build-ubsan = mkShitty pkgs { sanitizer = "ubsan"; };
          tests-ubsan = mkTestCheck pkgs { sanitizer = "ubsan"; };
        }
      );

      formatter = forAllSystems (system: (nixpkgsFor system).nixfmt);

      # Overlay so consumers can `pkgs.shitty` after importing the flake.
      overlays.default = final: prev: {
        shitty = mkShitty final { };
      };
    };
}
