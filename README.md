# ECOBORA Smart Cooking HMI

> **Ported to LVGL 9.5.0.** See [LVGL 9.5 port notes](#lvgl-95-port-notes) at the bottom
> for what changed. The original text below describes the v8.4 baseline it came from.

This is the **real VIEWE `ESP32_Display_Panel` PlatformIO example** (from the
`Ecobora.zip` you uploaded, originally `esp-arduino-libs/ESP32_Display_Panel`'s
`examples/platformio/lvgl_v8_port`), trimmed to just the
**UEDX24320028E-WB-A** board env, with our 4 HMI screens wired into `main.cpp`
in place of the LVGL demo widgets.

Nothing here is hand-transcribed guesswork anymore — `platformio.ini`'s board
env, `boards/BOARD_VIEWE_UEDX24320028E_WB_A.json`, `lib/lv_conf.h`,
`src/lvgl_v8_port.{h,cpp}`, and `src/esp_panel_*_conf.h` are your real files,
copied over unmodified. I only changed two things from the original:
1. Trimmed `platformio.ini` down to the one board env we're using (the rest are commented out in your original — easy to bring back if you switch panels).
2. Trimmed `lib/lvgl-release-v8.4/` down to just `src/` + top-level headers (removed `demos/`, `examples/`, `tests/`, `docs/`, `scripts/`, `env_support/` — none of that is referenced anywhere in our build, and it cut the folder from 169MB to 16MB so this is actually deliverable as a chat attachment). If you want the untouched upstream copy, re-copy `lib/lvgl-release-v8.4` from your original `Ecobora.zip` over this one — it changes nothing else.

## What's ours vs. what's VIEWE's

| Ours | Theirs (unmodified) |
|---|---|
| `src/main.cpp` (adapted — same board bring-up sequence, swapped `lv_demo_widgets()` for `screen_manager_init()`) | `boards/*.json` |
| `src/ui/` — all 4 screens, theme, header, screen manager | `lib/lv_conf.h` |
| `platformio.ini` (trimmed to 1 env, values unchanged) | `src/lvgl_v8_port.h` / `.cpp` |
| | `src/esp_panel_board_custom_conf.h` |
| | `src/esp_panel_drivers_conf.h` |
| | `src/esp_utils_conf.h` |
| | `lib/lvgl-release-v8.4/` (trimmed, not modified) |

## Build & flash

```
pio run -e BOARD_VIEWE_UEDX24320028E_WB_A -t upload
pio device monitor -b 115200
```

If it doesn't auto-enter download mode, hold **BOOT** while plugging in the
USB-C cable.

Note the `[env]` in `platformio.ini` pulls `ESP32_Display_Panel`,
`ESP32_IO_Expander`, and `esp-lib-utils` straight from their GitHub repos (not
the PlatformIO registry) — that's how your original file had it, so first
build will take a bit longer while it clones those.

## What you'll see

Boots to the **Splash screen**. Tap anywhere to advance:
Splash → Main Menu (tap START COOKING or anywhere) → Active Cooking (tap
anywhere) → Session Complete (tap either button) → back to Main Menu.

This confirms display + touch both work before any real logic is wired in.

## Known simplifications (unchanged from before)

- Icons are LVGL built-in glyphs (`LV_SYMBOL_WIFI`, `LV_SYMBOL_POWER`, etc.),
  not the custom meal icons from the brand mockups — swap these once your
  EEZ Studio icon assets are ready.
- Meal grid buttons don't track a selection yet; screen transitions in
  `screen_manager.cpp` are just "any tap advances" for bring-up testing.
- Active Cooking's timer/progress/meal name (`"79:59"`, `62%`, `"BEANS"`) are
  hardcoded placeholders — no real countdown runs yet.
- I still haven't been able to compile this myself — no ESP32 toolchain or
  physical board in this environment. First build is likely to surface small
  things (an unused-variable warning, an include order issue) — send me the
  compiler output if `pio run` doesn't go clean and I'll fix it.

## Orientation check

The panel is native 240×320 (portrait). Our screens assume 320×240
(landscape). This board's SPI interface means the RGB-panel tearing/rotation
macros in `lvgl_v8_port.h` don't apply (they're gated to RGB/MIPI-DSI only),
so landscape orientation should come from the board driver's own default
config. **If the first boot shows the UI rotated or cropped**, that's the
one thing to flag — the fix is a `swapXY`/`mirror` call on the LCD object in
`main.cpp` after `board->begin()`, but which axes need it depends on what you
actually see on the panel, so I'd rather adjust it against a real photo/video
than guess blind.

## Not yet in scope (per the ToR, tracked for later)

Power switching / relay control, MQTT to Emissionfy, local data persistence
for offline queuing, RTC/timestamp handling across power loss, and the Dry
Run / Free Run state logic itself.


## LVGL 9.5 port notes

The project now builds against **LVGL v9.5.0**, bundled in `lib/lvgl-9.5.0/`
(the upstream `v9.5.0` tag, trimmed the same way as before: just `src/` plus the
top-level headers and metadata; no `demos/`, `examples/`, `tests/`, `docs/`).

| File | Change |
|---|---|
| `lib/lvgl-release-v8.4/` | Removed, replaced by `lib/lvgl-9.5.0/` |
| `lib/lv_conf.h` | Regenerated from the v9.5.0 template, with your v8 settings carried over: 16-bit color, libc `malloc`, logging via `printf` at WARN, perf monitor (now `LV_USE_SYSMON` + `LV_USE_PERF_MONITOR`), all Montserrat 8–48 fonts, 30 ms refresh. `LV_COLOR_SCREEN_TRANSP` has no v9 equivalent and was dropped. Demos and examples are off because those folders aren't bundled. |
| `platformio.ini` | Removed the `lvgl/lvgl@~9.5` registry dep, since LVGL is bundled and two copies would clash. `-DLV_COLOR_16_SWAP=1` stays: LVGL 9.5 still honours it and byte-swaps RGB565 before `flush_cb`. |
| `src/esp_panel/lvgl_v8_port.*` | Replaced by `lvgl_v9_port.{h,cpp}`, with the same public API (`lvgl_port_init/lock/unlock/deinit`). |
| `Include/main.h` | Includes `lvgl_v9_port.h`. |
| `src/main.cpp` | `lv_disp_set_rotation(..., LV_DISP_ROT_90)` → `lv_display_set_rotation(..., LV_DISPLAY_ROTATION_90)`. Removed the extra `lv_timer_handler()` from `loop()`, because the port's own `lvgl` task already runs it. |
| `src/ui/*` (EEZ) | `lv_scr_load_anim` → `lv_screen_load_anim`, `lv_disp_*` → `lv_display_*`, `lv_img_dsc_t` → `lv_image_dsc_t`. |

### What the v9 port layer does differently

- `lv_display_create()` + `lv_display_set_buffers()`. Buffer sizes are now in
  **bytes**, and `sizeof(lv_color_t)` is 3 in v9, so pixel size comes from `LV_COLOR_DEPTH`.
- Rotation: if the panel supports swap/mirror (this GC9307 does), it's applied on
  `LV_EVENT_RESOLUTION_CHANGED`, the v9 replacement for `drv_update_cb`.
  Otherwise the flush rotates in software with `lv_draw_sw_rotate()`.
  Touch coordinates are rotated by LVGL itself.
- Rounder → `LV_EVENT_INVALIDATE_AREA` event; tick → `lv_tick_set_cb()` on `esp_timer`.
- The RGB/MIPI-DSI anti-tearing modes are ported too. They aren't used by this
  SPI board and haven't been tested on hardware.

### Regenerating the UI in EEZ Studio

Set **LVGL version → 9.x** in the EEZ project settings before you rebuild. If it's
left on 8.x, EEZ will overwrite `src/ui/` with v8 calls again.
