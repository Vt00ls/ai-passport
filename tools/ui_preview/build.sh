#!/bin/bash
# Build and run the off-device render harness for the dashboard UI.
#
# Compiles the firmware's own main/app_dashboard/*.c against host LVGL and writes one
# BMP per UI state into tools/ui_preview/build/. No device, no flashing, no display.
#
# Requires the LVGL sources that the firmware build fetches, so run
# `./tools/validate.sh --firmware` (or any `idf.py build`) at least once first.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
REPO=$(cd -- "$HERE/../.." && pwd)
LVGL="$REPO/managed_components/lvgl__lvgl"
OBJ="$HERE/build/obj"
OUT="$HERE/build"
CC=${CC:-cc}

if [[ ! -d "$LVGL/src" ]]; then
    echo "ERROR: $LVGL is missing. Run ./tools/validate.sh --firmware once so the" >&2
    echo "       component manager fetches LVGL, then re-run this script." >&2
    exit 1
fi

mkdir -p "$OBJ" "$OUT"

CFLAGS=(-std=gnu17 -O1 -w -DLV_CONF_INCLUDE_SIMPLE
        -I"$HERE" -I"$HERE/stubs" -I"$REPO/main" -I"$REPO/components/bsp/include"
        -I"$LVGL" -I"$LVGL/src")

echo "compiling LVGL (cached; first run is slow)..."
find "$LVGL/src" -name '*.c' | while read -r src; do
    obj="$OBJ/lv_$(echo "${src#$LVGL/src/}" | tr '/' '_' | sed 's/\.c$/.o/')"
    [[ -f "$obj" ]] || "$CC" "${CFLAGS[@]}" -c "$src" -o "$obj"
done

echo "compiling the dashboard sources under test..."
for unit in dashboard_ui dashboard_health dashboard_format dashboard_protocol; do
    "$CC" "${CFLAGS[@]}" -c "$REPO/main/app_dashboard/$unit.c" -o "$OBJ/dash_$unit.o"
done
"$CC" "${CFLAGS[@]}" -c "$HERE/harness/render_states.c" -o "$OBJ/render_states.o"

echo "linking..."
"$CC" -o "$OUT/render_states" "$OBJ"/*.o -lm

cd "$OUT" && "$OUT/render_states"
echo
echo "Images written to $OUT"
