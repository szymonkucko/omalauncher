#!/bin/sh
# Zero-copy P0: builds the guest client, oma-zc-client, for aarch64 Linux (glibc 2.35 and newer)
# in a container and puts it into the debug assets, where ZeroCopyP0Activity copies it into the guest.
# Needs docker with arm64 images (native on Apple silicon). The asset is gitignored; debug builds
# without it still build, and the P0 activity says the client is missing.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out="$here/build"
docker build --platform linux/arm64 -q -t oma-zc-build "$here" >/dev/null
docker run --rm --platform linux/arm64 -v "$root:/src" -w /src/scripts/zero-copy oma-zc-build sh -euc '
  mkdir -p build
  glslangValidator -V --quiet --vn zc_vert_spv -o build/zc_vert.h zc.vert
  glslangValidator -V --quiet --vn zc_frag_spv -o build/zc_frag.h zc.frag
  gcc -O2 -g -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -include stdint.h \
      -I build -I /src/app/src/debug/cpp -o build/oma-zc-client zc_client.c -ldl
  file build/oma-zc-client'
mkdir -p "$root/app/src/debug/assets/zc"
cp "$out/oma-zc-client" "$root/app/src/debug/assets/zc/oma-zc-client"
echo "copied to app/src/debug/assets/zc/oma-zc-client"
