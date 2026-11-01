#!/usr/bin/env bash
# Build the release configuration and package it.
# Usage: package-release.sh <version>
# Output in dist/: hygeia.bin (app image for web updates), hygeia-full.hex
# (Intel HEX of bootloader, partition table, otadata and app for first-time
# flashing; it has gaps, so flashing it never writes nvs or storage), SHA256SUMS.
set -euo pipefail

ver="$1"

cat > sdkconfig.version <<CFG
CONFIG_APP_PROJECT_VER_FROM_CONFIG=y
CONFIG_APP_PROJECT_VER="${ver}"
CFG

idf.py -B build-rel \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release;sdkconfig.version" build
! grep -qw console_start build-rel/hygeia.map

mkdir -p dist
cp build-rel/hygeia.bin dist/hygeia.bin
(cd build-rel && python -m esptool --chip esp32s3 merge_bin \
  --format hex -o ../dist/hygeia-full.hex @flash_args)
(cd dist && sha256sum hygeia.bin hygeia-full.hex > SHA256SUMS && cat SHA256SUMS)
