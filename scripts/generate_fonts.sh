#!/usr/bin/env bash
# Generates the firmware's Chinese-capable fonts.
#
# nav_16 / nav_20 cover the UI strings plus GB2312 level-1 (3755 common hanzi),
# so music titles and road names coming from the phone render offline.
# nav_28 only needs the UI strings (headline text); keeping it small avoids
# spending flash on glyphs that never appear at that size.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_dir}"

font="third_party/lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
symbols_cn="$(cat scripts/font_symbols_cn.txt)"
symbols_ui="$(cat scripts/font_symbols_ui.txt)"

gen() {
  local size="$1"
  local symbols="$2"
  local output="$3"
  npx --yes lv_font_conv \
    --font "${font}" \
    --size "${size}" \
    --bpp 4 \
    --format lvgl \
    --no-compress \
    --range 0x20-0x7E \
    --symbols "${symbols}" \
    -o "${output}"
}

gen 16 "${symbols_cn}" shared/nav_ui/assets/moto_font_nav_16.c
gen 20 "${symbols_cn}" shared/nav_ui/assets/moto_font_nav_20.c
gen 28 "${symbols_ui}" shared/nav_ui/assets/moto_font_nav_28.c
