#!/bin/sh
set -eu
cd "$(dirname "$0")/../../.."
export PKG_CONFIG_PATH="$PWD/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
for name in overview_render input_mode; do
    ${CC:-cc} -std=c11 -D_GNU_SOURCE -DENABLE_DRM=1 -O1 -g ${TEST_CFLAGS:-} -ffunction-sections -fdata-sections \
        -Wl,--gc-sections -Isrc/neuswc/libswc \
        $(pkg-config --cflags wld wayland-server xkbcommon) \
        "src/neuswc/test/${name}_test.c" \
        -o "src/neuswc/build/${name}_test" \
        $(pkg-config --libs --static wld wayland-server xkbcommon) -lm
    "src/neuswc/build/${name}_test"
done
