#!/bin/sh
# Regenerates the LVGL font sources from Inter (SIL OFL 1.1, see OFL.txt).
# Needs node/npx and network access. Run from this directory.
# One file per entry of the type scale (components/hy_ui/src/theme.c).
set -e
tmp=$(mktemp -d)
(cd "$tmp" && npm pack @fontsource/inter@5.3.0 >/dev/null && tar xzf fontsource-inter-5.3.0.tgz)
# Latin set: ASCII, Latin-1, em dash, ellipsis, single guillemets, minus sign
LATIN="0x20-0x7F,0xA0-0xFF,0x2014,0x2026,0x2039,0x203A,0x2212"
gen() { # name weight size range
  npx --yes lv_font_conv@1.5.3 --font "$tmp/package/files/inter-latin-$2-normal.woff" -r "$4" \
    --size "$3" --bpp 4 --format lvgl --lv-include lvgl.h --no-compress \
    --lv-font-name "$1" -o "$1.c"
  sed -i "s|--font [^ ]*|--font Inter-$2.woff|" "$1.c" # keep temp paths out of the sources
}
gen hy_inter_14 400 14 "$LATIN"            # small
gen hy_inter_17 400 17 "$LATIN"            # status
gen hy_inter_b17 700 17 "$LATIN"           # status_bold
gen hy_inter_22 400 22 "$LATIN"            # body
gen hy_inter_b22 700 22 "$LATIN"           # body_bold
gen hy_inter_b26 700 26 "$LATIN"           # title
gen hy_inter_b30 700 30 "$LATIN"           # time
gen hy_inter_b38 700 38 "0x30-0x39,0x2F"   # count: digits and "/"
gen hy_inter_b44 700 44 "$LATIN"           # code_m
gen hy_inter_b52 700 52 "$LATIN"           # code
gen hy_inter_b96 700 96 "0x41-0x5A,0x30-0x39" # code_l: A-Z 0-9
gen hy_inter_l120 200 120 "0x30-0x3A,0x2D" # clock: weight 200, 0-9 ":" "-"
rm -rf "$tmp"
