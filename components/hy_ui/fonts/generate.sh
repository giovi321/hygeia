#!/bin/sh
# Regenerates the LVGL font sources from Inter (SIL OFL 1.1, see OFL.txt).
# Needs node/npx and network access. Run from this directory.
set -e
tmp=$(mktemp -d)
(cd "$tmp" && npm pack @fontsource/inter@5.3.0 >/dev/null && tar xzf fontsource-inter-5.3.0.tgz)
R="0x20-0x7F,0xA0-0xFF,0x2014"
gen() { # name weight size
  npx --yes lv_font_conv@1.5.3 --font "$tmp/package/files/inter-latin-$2-normal.woff" -r "$R" \
    --size "$3" --bpp 4 --format lvgl --lv-include lvgl.h --no-compress \
    --lv-font-name "$1" -o "$1.c"
  sed -i "s|--font [^ ]*|--font Inter-$2.woff|" "$1.c" # keep temp paths out of the sources
}
gen hy_inter_17 400 17
gen hy_inter_22 400 22
gen hy_inter_b17 700 17
gen hy_inter_b22 700 22
gen hy_inter_b30 700 30
gen hy_inter_b52 700 52
rm -rf "$tmp"
