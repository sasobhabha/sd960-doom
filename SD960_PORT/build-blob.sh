#!/bin/bash
# Build the standalone DOOM blob (DOOM.BIN) for Canon PowerShot cameras.
# Takes one optional argument: the raw buffer address (e.g. 0x02800000 for DIGIC 4).
# If omitted, defaults to 0x00E80000 (SD1000).

BLOB_ADDR=${1:-0x00E80000}

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -n "$XPACK" ]; then export PATH="${XPACK%/}/bin:$PATH"; fi
command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc not found (set XPACK= or add to PATH)"; exit 1; }
TC="$(dirname "$(command -v arm-none-eabi-gcc)")/.."

SRC="$HERE/build/doomgeneric/doomgeneric"
[ -d "$SRC" ] || git clone --depth 1 https://github.com/ozkl/doomgeneric "$HERE/build/doomgeneric"
cp "$HERE"/src/doom_chdk_abi.h "$HERE"/src/doomgeneric_chdk.c "$HERE"/src/chdk_syscalls.c "$HERE"/src/doom_printf.c "$SRC/"

# Generate linker script with the correct BLOB_ADDR
sed "s/0x00E80000/$BLOB_ADDR/g" "$HERE/src/doom_blob.ld" > "$SRC/doom_blob_gen.ld"

cd "$SRC"
OUT="$HERE/build/obj"; rm -rf "$OUT"; mkdir -p "$OUT"
CF="-c -march=armv5te -mtune=arm946e-s -Os -fno-strict-aliasing -fcommon -DNORMALUNIX -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200"
for c in $(ls *.c | grep -vE "doomgeneric_(xlib|sdl|sosox?|emscripten|fb|kitty|curses|allegro|linuxvt|win)\.c|i_allegro|i_sdl"); do
  arm-none-eabi-gcc $CF -o "$OUT/${c%.c}.o" "$c"
done

LIBC="$TC/arm-none-eabi/lib/arm/v5te/softfp/libc.a"
LIBGCC="$(ls "$TC"/lib/gcc/arm-none-eabi/*/arm/v5te/softfp/libgcc.a | head -1)"
arm-none-eabi-gcc -nostartfiles -nostdlib -Wl,-T,doom_blob_gen.ld -Wl,-Map,"$OUT/doom_blob.map" \
  -o "$OUT/doom_blob.elf" -Wl,--start-group "$OUT"/*.o "$LIBC" "$LIBGCC" -Wl,--end-group
arm-none-eabi-objcopy -O binary "$OUT/doom_blob.elf" "$HERE/DOOM.BIN"
echo "built DOOM.BIN linked at $BLOB_ADDR:"
ls -la "$HERE/DOOM.BIN"
arm-none-eabi-nm "$OUT/doom_blob.elf" | grep -iE " doom_main$|__bss_start|__bss_end|_blob_end"
