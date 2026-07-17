# DOOM Port for Canon PowerShot SD960 (IXUS 110 IS)

I've modified the core components of the SD1000 DOOM port to dynamically adapt to the SD960 (DIGIC 4).

## 1. Dynamic RAW Address Probing & Relinking
Since we don't know the exact `hook_raw_image_addr()` for the SD960 ahead of time (and it varies slightly across firmware versions), I rewrote the launcher (`gui_doom.c`) to act as a **dynamic probe**:
- Instead of hardcoding `BLOB_ADDR` in the C code, it now dynamically reads `hook_raw_image_addr()` and sets `blob_addr`.
- **How to use it:** Compile and load the new launcher (`doom.flt`) onto your SD960 *without* `DOOM.BIN` on the card. Run it, and it will print the exact probed RAW address to the screen!
- Once you have the address (e.g., `0x02800000`), you pass it to the updated build script:
  ```bash
  ./build-blob.sh 0x02800000
  ```
- The build script now uses `sed` to generate a dynamic linker script (`doom_blob_gen.ld`) that relinks the blob to that exact physical address.

## 2. DIGIC 4 UYVY Viewport Rendering
The SD1000 used a 6-byte packed YUV format (`UYVYYY`). The SD960, being a DIGIC 4 camera, uses the standard 4:2:2 `UYVY` format (4 bytes for 2 pixels).
- I updated the `cb_draw` loop to decode 4-byte UYVY groupings.
- I also replaced the hardcoded SD1000 viewport variables with CHDK's exported dynamic APIs (`vid_get_viewport_live_fb()`, `vid_get_viewport_width()`, etc.), meaning it will adapt automatically to the SD960's wider screen resolution without breaking the aspect ratio.
- The scale buffers `g_hx` and `g_vy` were resized to accommodate the larger DIGIC 4 screens.

## 3. SD960 Keymap
The SD960 lacks a D-Pad and a Print button. It relies on a multi-function scroll wheel and a dedicated Playback button. 
- I remapped movement to the scroll wheel edges (`KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`).
- The center button (`KEY_SET`) is still used for firing/doors.
- **Quit Game:** Since there is no `PRINT` button, `KEY_PLAYBACK` is now polled to instantly kill the blob and exit to CHDK.

### Next Steps
1. Recompile the launcher (`doom.flt`) inside your CHDK tree for `ixus110_sd960`.
2. Load it onto the camera and run it to get the RAW address.
3. Re-run `./build-blob.sh [YOUR_ADDR]`.
4. Copy `DOOM.BIN` to your card and enjoy!

Let me know if you run into any issues with the viewport alignment once you boot it up!
