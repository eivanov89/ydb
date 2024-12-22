pkgs: attrs: with pkgs; with attrs; rec {
  version = "17.2";
  version_with_underscores = "${lib.replaceStrings ["."] ["_"] version}";

  src = fetchFromGitHub {
    owner = "postgres";
    repo = "postgres";
    rev = "REL_${version_with_underscores}";
    hash = "sha256-P7IwvMcOI6vW14PiB2R0NEzAEPeaKg0zaUKTw2GJ5DA=";
  };

  patches = [];

  nativeBuildInputs = [
    bison
    flex
    perl
    pkg-config
  ];

  buildPhase = ''
    make -j$NIX_BUILD_CORES submake-generated-headers
    make -j$NIX_BUILD_CORES -C src/common
    make -j$NIX_BUILD_CORES -C src/port
    make -j$NIX_BUILD_CORES -C src/interfaces/libpq all-shared-lib
  '';

  configureFlags = attrs.configureFlags ++ ["--without-gssapi"];
}
