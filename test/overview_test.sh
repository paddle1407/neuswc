#!/bin/sh
# Compatibility entry point; the checks are now registered with Meson.
set -eu
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
BUILD_DIR=${SWC_BUILD_DIR:-$ROOT/build}
meson test -C "$BUILD_DIR" --print-errorlogs "$@"
