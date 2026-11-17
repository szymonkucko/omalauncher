#!/bin/sh
# Zero-copy: builds, for aarch64 Linux (glibc 2.35 and newer) in a container, the Vulkan layer
# (VK_LAYER_OMA_zero_copy) into app/src/main/assets/zero-copy, which the launcher installs in the guest
# (Gpu.managed; commit it with its source), and the test client (oma-zc-client) into the debug assets,
# where ZeroCopyP0Activity copies it into the guest (gitignored; without it debug builds still build).
# Needs docker with arm64 images (native on Apple silicon).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out="$here/build"
docker build --platform linux/arm64 -q -t oma-zc-build "$here" >/dev/null
docker run --rm --platform linux/arm64 -v "$root:/src" -w /src/scripts/zero-copy oma-zc-build sh -euc '
  mkdir -p build
  glslangValidator -V --quiet --vn zc_vert_spv -o build/zc_vert.h zc.vert
  glslangValidator -V --quiet --vn zc_frag_spv -o build/zc_frag.h zc.frag
  xdg=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
  wayland-scanner client-header $xdg build/xdg-shell-client-protocol.h
  wayland-scanner private-code $xdg build/xdg-shell-protocol.c
  wayland-scanner client-header oma-ahb-v1.xml build/oma-ahb-v1-client-protocol.h
  wayland-scanner private-code oma-ahb-v1.xml build/oma-ahb-v1-protocol.c
  gcc -O2 -g -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -D_GNU_SOURCE -include stdint.h \
      -I build -I /src/app/src/debug/cpp -o build/oma-zc-client zc_client.c \
      build/xdg-shell-protocol.c build/oma-ahb-v1-protocol.c -ldl -lwayland-client
  gcc -O2 -g -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -D_GNU_SOURCE \
      -shared -fPIC -fvisibility=hidden -Wl,--no-undefined -I build -o build/libVkLayer_oma_zero_copy.so \
      oma_zero_copy_layer.c build/oma-ahb-v1-protocol.c -lwayland-client -lpthread
  strip -o build/libVkLayer_oma_zero_copy.stripped.so build/libVkLayer_oma_zero_copy.so
  file build/oma-zc-client build/libVkLayer_oma_zero_copy.so
  nm -D --defined-only build/libVkLayer_oma_zero_copy.so | grep -i " T "'
mkdir -p "$root/app/src/debug/assets/zc"
cp "$out/oma-zc-client" "$root/app/src/debug/assets/zc/"
cp "$out/libVkLayer_oma_zero_copy.stripped.so" "$root/app/src/main/assets/zero-copy/libVkLayer_oma_zero_copy.so"
echo "copied oma-zc-client to app/src/debug/assets/zc/ and the layer to app/src/main/assets/zero-copy/"
