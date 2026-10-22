# viewe-2.8-lvgl9-lottie

LVGL 9.5.0 port and Lottie animation test for the Viewe UEDX24320028E-WB-A
(2.8", 240x320 SPI TFT, capacitive touch, ESP32-S3 with PSRAM).
Built with PlatformIO and the Arduino framework.

This repository is a source-only skeleton. LVGL itself is not included, see
[Setup](#setup).

## What is included

| Path | Purpose |
|---|---|
| `platformio.ini` | Build config for the Viewe board |
| `lib/lv_conf.h` | LVGL 9.5 config with Lottie, ThorVG and vector graphics enabled |
| `src/main.cpp` | Board, LVGL and UI start-up |
| `src/esp_panel/` | Board config and the LVGL v9 display and touch port |
| `src/ui/` | EEZ Studio generated UI |
| `src/lottie/lottie_player.*` | Creates a Lottie animation from a C array |
| `src/lottie/lottie_demo.*` | Example that plays the test animation |
| `tools/lottie_check.py` | Checks a Lottie file and suggests a size |
| `tools/lottie_to_c.py` | Converts a `.json` or `.lottie` file to a C array |
| `animations/` | Source animation files |

## Setup

1. Clone this repository.

2. Add LVGL 9.5.0 into `lib/lvgl-9.5.0`:
   ```
   git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git lib/lvgl-9.5.0
   ```
   Do not add `lvgl/lvgl` to `lib_deps`, or you will get two copies.

3. Patch ThorVG to avoid a stack overflow. In
   `lib/lvgl-9.5.0/src/libs/thorvg/tvgSwRle.cpp`, inside `rleRender`, change
   ```cpp
   Cell buffer[RENDER_POOL_SIZE / sizeof(Cell)];
   ```
   to
   ```cpp
   static Cell buffer[RENDER_POOL_SIZE / sizeof(Cell)];
   ```
   Without this, rendering needs about 19 KB of stack and the board reboots in a loop
   with a white screen. The change is safe because rendering is single threaded and
   runs under the LVGL lock.

4. Check the stack sizes (already set in this repository):
   - `LVGL_PORT_TASK_STACK_SIZE` is `24 * 1024` in `src/esp_panel/lvgl_v9_port.h`.
   - `SET_LOOP_TASK_STACK_SIZE(16 * 1024);` is at file level in `src/main.cpp`.

5. Build and flash:
   ```
   pio run -t clean
   pio run -t upload
   pio device monitor
   ```

## Using your own Lottie file

1. Put the file in `animations/`. It must be `.json` or `.lottie`.

2. Check it:
   ```
   python tools/lottie_check.py animations/my_anim.json
   ```
   It prints the fps, frame count and aspect ratio, a suggested width and height,
   and warns about features that do not work or run slowly (embedded images, text
   layers, expressions, effects, very large files).

3. Convert it to C. Use a different symbol name for each file:
   ```
   python tools/lottie_to_c.py animations/my_anim.json lottie_loader > src/lottie/lottie_loader.c
   ```

4. Create it in code, with the LVGL lock held:
   ```cpp
   extern "C" {
   extern const uint8_t lottie_loader[];
   extern const size_t lottie_loader_size;
   }
   #include "lottie/lottie_player.h"

   // width, height and fps come from lottie_check.py
   lv_obj_t *a = lottie_player_create(objects.main, lottie_loader, lottie_loader_size, 120, 120, 30);
   if (a) lv_obj_center(a);
   ```
   Keep width and height in the same ratio as the animation. Call `lv_obj_delete(a)`
   to remove it, which also frees its pixel buffer.

## Tips and troubleshooting

- Start at about 120x120 pixels. ThorVG redraws the whole animation every frame,
  so cost grows with pixel count.
- The pixel buffer is allocated in PSRAM (`width * height * 4` bytes).
- LVGL assumes 60 fps for Lottie. Pass the file's real fps to `lottie_player_create`
  or the animation plays at the wrong speed.
- White screen and reboot loop: check step 3 of Setup and the stack sizes.
- Animation too slow: reduce its on-screen size or simplify the file.
- Call `lottie_demo_report()` from `loop()` to print free heap, free PSRAM and the
  remaining LVGL task stack.
- Changed `lv_conf.h`? Run `pio run -t clean` before building.

## Notes

- Lottie here uses LVGL 9.5's built-in `lv_lottie` widget with ThorVG, not the
  separate rlottie library.
- `-DLV_COLOR_16_SWAP=1` in `platformio.ini` is required for this panel's SPI interface.
