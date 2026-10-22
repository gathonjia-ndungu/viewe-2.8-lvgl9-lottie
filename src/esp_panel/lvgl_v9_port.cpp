/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * LVGL v9.x port for ESP32_Display_Panel.
 *
 * This is Espressif's `lvgl_v8_port.cpp` ported to the LVGL 9.5 API. Behaviour is kept the same:
 *  - non-RGB (SPI/QSPI/I80) panels: partial render into 1-2 SRAM buffers, flush ready from the DMA-done ISR
 *  - RGB/MIPI-DSI panels: the optional "avoid tearing" modes (full-refresh / direct-mode on LCD frame buffers)
 *  - rotation via the panel's hardware swap/mirror when supported, otherwise software rotation in the flush
 *  - a dedicated FreeRTOS task running `lv_timer_handler()` protected by a recursive mutex
 *
 * Main v8 -> v9 changes in this file:
 *  - lv_disp_drv_t / lv_disp_draw_buf_t       -> lv_display_create() + lv_display_set_buffers() (size in BYTES)
 *  - flush_cb(lv_disp_drv_t*, ..., lv_color_t*) -> flush_cb(lv_display_t*, ..., uint8_t *px_map)
 *  - drv->rounder_cb                          -> LV_EVENT_INVALIDATE_AREA display event
 *  - drv->drv_update_cb (rotation)            -> LV_EVENT_RESOLUTION_CHANGED display event
 *  - drv->sw_rotate                           -> lv_draw_sw_rotate() inside the flush callback
 *  - lv_indev_drv_t                           -> lv_indev_create() + lv_indev_set_read_cb()
 *  - lv_tick_inc() from an esp_timer          -> lv_tick_set_cb() reading esp_timer_get_time()
 *  - sizeof(lv_color_t) is 3 in v9, so pixel sizes are derived from LV_COLOR_DEPTH instead
 *  - LV_COLOR_16_SWAP is no longer an lv_conf option; LVGL 9.5 still honours `-DLV_COLOR_16_SWAP=1`
 *    and swaps RGB565 bytes right before calling flush_cb, so the build flag keeps working.
 */

#include "freertos/FreeRTOS.h"

#include "esp_timer.h"
#include "esp_heap_caps.h"
#undef ESP_UTILS_LOG_TAG
#define ESP_UTILS_LOG_TAG "LvPort"
#include "esp_lib_utils.h"
#include "lvgl_v9_port.h"
// Private LVGL headers are needed for the anti-tearing modes (direct access to buffers / invalidated areas)
#include "lvgl_private.h"

#if LVGL_VERSION_MAJOR < 9
#error "This port requires LVGL v9.x"
#endif

using namespace esp_panel::drivers;

#define LVGL_PORT_ENABLE_ROTATION_OPTIMIZED     (1)
#define LVGL_PORT_BUFFER_NUM_MAX                (2)
#define LVGL_PORT_PIXEL_SIZE                    (LV_COLOR_DEPTH / 8)    // Bytes per pixel of the native color format

static SemaphoreHandle_t lvgl_mux = nullptr;                  // LVGL mutex
static TaskHandle_t lvgl_task_handle = nullptr;
static void *lvgl_buf[LVGL_PORT_BUFFER_NUM_MAX] = {};
static lv_display_t *lvgl_disp = nullptr;
static lv_indev_t *lvgl_indev = nullptr;

#if LVGL_PORT_ROTATION_DEGREE != 0
static void *get_next_frame_buffer(LCD *lcd)
{
    static void *next_fb = NULL;
    static void *fbs[2] = { NULL };

    if (next_fb == NULL) {
        fbs[0] = lcd->getFrameBufferByIndex(0);
        fbs[1] = lcd->getFrameBufferByIndex(1);
        next_fb = fbs[1];
    } else {
        next_fb = (next_fb == fbs[0]) ? fbs[1] : fbs[0];
    }

    return next_fb;
}

__attribute__((always_inline))
static inline void copy_pixel_8bpp(uint8_t *to, const uint8_t *from)
{
    *to++ = *from++;
}

__attribute__((always_inline))
static inline void copy_pixel_16bpp(uint8_t *to, const uint8_t *from)
{
    *(uint16_t *)to++ = *(const uint16_t *)from++;
}

__attribute__((always_inline))
static inline void copy_pixel_24bpp(uint8_t *to, const uint8_t *from)
{
    *to++ = *from++;
    *to++ = *from++;
    *to++ = *from++;
}

__attribute__((always_inline))
static inline void copy_pixel_32bpp(uint8_t *to, const uint8_t *from)
{
    *(uint32_t *)to = *(const uint32_t *)from;
}

#define _COPY_PIXEL(_bpp, to, from) copy_pixel_##_bpp##bpp(to, from)
#define COPY_PIXEL(_bpp, to, from)  _COPY_PIXEL(_bpp, to, from)

#define ROTATE_90_ALL_BPP() \
    { \
        to_bytes_per_line = h * to_bytes_per_piexl; \
        to_index_const = (w - x_start - 1) * to_bytes_per_line; \
        for (int from_y = y_start; from_y < y_end + 1; from_y++) { \
            from_index = from_y * from_bytes_per_line + x_start * from_bytes_per_piexl; \
            to_index = to_index_const + from_y * to_bytes_per_piexl; \
            for (int from_x = x_start; from_x < x_end + 1; from_x++) { \
                COPY_PIXEL(LV_COLOR_DEPTH, to + to_index, from + from_index); \
                from_index += from_bytes_per_piexl; \
                to_index -= to_bytes_per_line; \
            } \
        } \
    }

/**
 * @brief Optimized transpose function for RGB565 format.
 *
 * @note  ESP32-P4 1024x600 full-screen: 738ms -> 34ms
 * @note  ESP32-S3 480x480  full-screen: 380ms -> 37ms
 */
#define ROTATE_90_OPTIMIZED_16BPP(block_w, block_h) \
    { \
        for (int i = 0; i < h; i += block_h) { \
            max_height = (i + block_h > h) ? h : (i + block_h); \
            for (int j = 0; j < w; j += block_w) { \
                max_width = (j + block_w > w) ? w : (j + block_w); \
                start_y = w - 1 - j;   \
                for (int x = i; x < max_height; x++) { \
                    from_next = (uint16_t *)from + x * w; \
                    for (int y = j, mirrored_y = start_y; y < max_width; y += 4, mirrored_y -= 4) { \
                        ((uint16_t *)to)[(mirrored_y) * h + x] = *((uint32_t *)(from_next + y)) & 0xFFFF; \
                        ((uint16_t *)to)[(mirrored_y - 1) * h + x] = (*((uint32_t *)(from_next + y)) >> 16) & 0xFFFF; \
                        ((uint16_t *)to)[(mirrored_y - 2) * h + x] = *((uint32_t *)(from_next + y + 2)) & 0xFFFF; \
                        ((uint16_t *)to)[(mirrored_y - 3) * h + x] = (*((uint32_t *)(from_next + y + 2)) >> 16) & 0xFFFF; \
                    } \
                } \
            } \
        } \
    }

#define ROTATE_180_ALL_BPP() \
    { \
        to_bytes_per_line = w * to_bytes_per_piexl; \
        to_index_const = (h - 1) * to_bytes_per_line + (w - x_start - 1) * to_bytes_per_piexl; \
        for (int from_y = y_start; from_y < y_end + 1; from_y++) { \
            from_index = from_y * from_bytes_per_line + x_start * from_bytes_per_piexl; \
            to_index = to_index_const - from_y * to_bytes_per_line; \
            for (int from_x = x_start; from_x < x_end + 1; from_x++) { \
                COPY_PIXEL(LV_COLOR_DEPTH, to + to_index, from + from_index); \
                from_index += from_bytes_per_piexl; \
                to_index -= to_bytes_per_piexl; \
            } \
        } \
    }

#define ROTATE_270_OPTIMIZED_16BPP(block_w, block_h) \
    { \
        for (int i = 0; i < h; i += block_h) { \
            max_height = i + block_h > h ? h : i + block_h; \
            for (int j = 0; j < w; j += block_w) { \
                max_width = j + block_w > w ? w : j + block_w; \
                for (int x = i; x < max_height; x++) { \
                    from_next = (uint16_t *)from + x * w; \
                    for (int y = j; y < max_width; y += 4) { \
                        ((uint16_t *)to)[y * h + (h - 1 - x)] = *((uint32_t *)(from_next + y)) & 0xFFFF; \
                        ((uint16_t *)to)[(y + 1) * h + (h - 1 - x)] = (*((uint32_t *)(from_next + y)) >> 16) & 0xFFFF; \
                        ((uint16_t *)to)[(y + 2) * h + (h - 1 - x)] = *((uint32_t *)(from_next + y + 2)) & 0xFFFF; \
                        ((uint16_t *)to)[(y + 3) * h + (h - 1 - x)] = (*((uint32_t *)(from_next + y + 2)) >> 16) & 0xFFFF; \
                    } \
                } \
            } \
        } \
    }

#define ROTATE_270_ALL_BPP() \
    { \
        to_bytes_per_line = h * to_bytes_per_piexl; \
        from_index_const = x_start * from_bytes_per_piexl; \
        to_index_const = x_start * to_bytes_per_line + (h - 1) * to_bytes_per_piexl; \
        for (int from_y = y_start; from_y < y_end + 1; from_y++) { \
            from_index = from_y * from_bytes_per_line + from_index_const; \
            to_index = to_index_const - from_y * to_bytes_per_piexl; \
            for (int from_x = x_start; from_x < x_end + 1; from_x++) { \
                COPY_PIXEL(LV_COLOR_DEPTH, to + to_index, from + from_index); \
                from_index += from_bytes_per_piexl; \
                to_index += to_bytes_per_line; \
            } \
        } \
    }

__attribute__((always_inline))
IRAM_ATTR static inline void rotate_copy_pixel(
    const uint8_t *from, uint8_t *to, uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end, uint16_t w,
    uint16_t h, uint16_t rotate
)
{
    // NOTE: v8 used `sizeof(lv_color_t)` here. In v9 `lv_color_t` is always RGB888 (3 bytes),
    //       while the render buffer uses the native color format, so derive the size from LV_COLOR_DEPTH.
    int from_bytes_per_piexl = LVGL_PORT_PIXEL_SIZE;
    int from_bytes_per_line = w * from_bytes_per_piexl;
    int from_index = 0;

    int to_bytes_per_piexl = LVGL_PORT_PIXEL_SIZE;
    int to_bytes_per_line;
    int to_index = 0;
    int to_index_const = 0;

#if (LV_COLOR_DEPTH == 16) && LVGL_PORT_ENABLE_ROTATION_OPTIMIZED
    int max_height = 0;
    int max_width = 0;
    int start_y = 0;
    uint16_t *from_next = NULL;
#endif

    switch (rotate) {
    case 90:
#if (LV_COLOR_DEPTH == 16) && LVGL_PORT_ENABLE_ROTATION_OPTIMIZED
        ROTATE_90_OPTIMIZED_16BPP(32, 256);
#else
        ROTATE_90_ALL_BPP();
#endif
        break;
    case 180:
        ROTATE_180_ALL_BPP();
        break;
    case 270: {
#if (LV_COLOR_DEPTH == 16) && LVGL_PORT_ENABLE_ROTATION_OPTIMIZED
        ROTATE_270_OPTIMIZED_16BPP(32, 256);
#else
        int from_index_const = 0;
        ROTATE_270_ALL_BPP();
#endif
        break;
    }
    default:
        break;
    }
}
#endif /* LVGL_PORT_ROTATION_DEGREE */

#if LVGL_PORT_AVOID_TEAR
#if LVGL_PORT_DIRECT_MODE
#if LVGL_PORT_ROTATION_DEGREE != 0
typedef struct {
    uint16_t inv_p;
    uint8_t inv_area_joined[LV_INV_BUF_SIZE];
    lv_area_t inv_areas[LV_INV_BUF_SIZE];
} lv_port_dirty_area_t;

static lv_port_dirty_area_t dirty_area;

static void flush_dirty_save(lv_port_dirty_area_t *dirty_area)
{
    lv_display_t *disp = lv_refr_get_disp_refreshing();
    dirty_area->inv_p = disp->inv_p;
    for (int i = 0; i < (int)disp->inv_p; i++) {
        dirty_area->inv_area_joined[i] = disp->inv_area_joined[i];
        dirty_area->inv_areas[i] = disp->inv_areas[i];
    }
}

typedef enum {
    FLUSH_STATUS_PART,
    FLUSH_STATUS_FULL
} lv_port_flush_status_t;

typedef enum {
    FLUSH_PROBE_PART_COPY,
    FLUSH_PROBE_SKIP_COPY,
    FLUSH_PROBE_FULL_COPY,
} lv_port_flush_probe_t;

/**
 * @brief Probe dirty area to copy
 *
 * @note This function is used to avoid tearing effect, and only work with LVGL direct-mode.
 */
static lv_port_flush_probe_t flush_copy_probe(lv_display_t *disp)
{
    static lv_port_flush_status_t prev_status = FLUSH_STATUS_PART;
    lv_port_flush_status_t cur_status;
    lv_port_flush_probe_t probe_result;
    lv_display_t *disp_refr = lv_refr_get_disp_refreshing();

    uint32_t flush_ver = 0;
    uint32_t flush_hor = 0;
    for (int i = 0; i < (int)disp_refr->inv_p; i++) {
        if (disp_refr->inv_area_joined[i] == 0) {
            flush_ver = (disp_refr->inv_areas[i].y2 + 1 - disp_refr->inv_areas[i].y1);
            flush_hor = (disp_refr->inv_areas[i].x2 + 1 - disp_refr->inv_areas[i].x1);
            break;
        }
    }
    /* Check if the current full screen refreshes */
    cur_status = ((flush_ver == (uint32_t)lv_display_get_vertical_resolution(disp)) &&
                  (flush_hor == (uint32_t)lv_display_get_horizontal_resolution(disp))) ?
                 (FLUSH_STATUS_FULL) : (FLUSH_STATUS_PART);

    if (prev_status == FLUSH_STATUS_FULL) {
        if ((cur_status == FLUSH_STATUS_PART)) {
            probe_result = FLUSH_PROBE_FULL_COPY;
        } else {
            probe_result = FLUSH_PROBE_SKIP_COPY;
        }
    } else {
        probe_result = FLUSH_PROBE_PART_COPY;
    }
    prev_status = cur_status;

    return probe_result;
}

static inline void *flush_get_next_buf(LCD *lcd)
{
    return get_next_frame_buffer(lcd);
}

/**
 * @brief Copy dirty area
 *
 * @note This function is used to avoid tearing effect, and only work with LVGL direct-mode.
 */
static void flush_dirty_copy(void *dst, void *src, lv_port_dirty_area_t *dirty_area)
{
    int32_t x_start, x_end, y_start, y_end;
    for (int i = 0; i < dirty_area->inv_p; i++) {
        /* Refresh the unjoined areas*/
        if (dirty_area->inv_area_joined[i] == 0) {
            x_start = dirty_area->inv_areas[i].x1;
            x_end = dirty_area->inv_areas[i].x2;
            y_start = dirty_area->inv_areas[i].y1;
            y_end = dirty_area->inv_areas[i].y2;

            rotate_copy_pixel(
                (uint8_t *)src, (uint8_t *)dst, x_start, y_start, x_end, y_end, LV_HOR_RES, LV_VER_RES,
                LVGL_PORT_ROTATION_DEGREE
            );
        }
    }
}

static void flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LCD *lcd = (LCD *)lv_display_get_user_data(disp);
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;
    void *next_fb = NULL;
    lv_port_flush_probe_t probe_result = FLUSH_PROBE_PART_COPY;

    /* Action after last area refresh */
    if (lv_display_flush_is_last(disp)) {
        /* Check if the full-refresh mode has been triggered (v8: `drv->full_refresh`) */
        if (disp->render_mode == LV_DISPLAY_RENDER_MODE_FULL) {
            /* Go back to direct mode */
            disp->render_mode = LV_DISPLAY_RENDER_MODE_DIRECT;

            // Rotate and copy data from the whole screen LVGL's buffer to the next frame buffer
            next_fb = flush_get_next_buf(lcd);
            rotate_copy_pixel(
                (uint8_t *)px_map, (uint8_t *)next_fb, offsetx1, offsety1, offsetx2, offsety2,
                LV_HOR_RES, LV_VER_RES, LVGL_PORT_ROTATION_DEGREE
            );

            /* Switch the current LCD frame buffer to `next_fb` */
            lcd->switchFrameBufferTo(next_fb);

            /* Waiting for the current frame buffer to complete transmission */
            ulTaskNotifyValueClear(NULL, ULONG_MAX);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            /* Synchronously update the dirty area for another frame buffer */
            flush_dirty_copy(flush_get_next_buf(lcd), px_map, &dirty_area);
            flush_get_next_buf(lcd);
        } else {
            /* Probe the copy method for the current dirty area */
            probe_result = flush_copy_probe(disp);

            if (probe_result == FLUSH_PROBE_FULL_COPY) {
                /* Save current dirty area for next frame buffer */
                flush_dirty_save(&dirty_area);

                /* Switch LVGL to full-refresh mode and set flush ready in advance */
                disp->render_mode = LV_DISPLAY_RENDER_MODE_FULL;
                disp->rendering_in_progress = false;
                lv_display_flush_ready(disp);

                /* Force to refresh whole screen, and will invoke `flush_callback` recursively */
                lv_refr_now(lv_refr_get_disp_refreshing());
            } else {
                /* Update current dirty area for next frame buffer */
                next_fb = flush_get_next_buf(lcd);
                flush_dirty_save(&dirty_area);
                flush_dirty_copy(next_fb, px_map, &dirty_area);

                /* Switch the current LCD frame buffer to `next_fb` */
                lcd->switchFrameBufferTo(next_fb);

                /* Waiting for the current frame buffer to complete transmission */
                ulTaskNotifyValueClear(NULL, ULONG_MAX);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

                if (probe_result == FLUSH_PROBE_PART_COPY) {
                    /* Synchronously update the dirty area for another frame buffer */
                    flush_dirty_save(&dirty_area);
                    flush_dirty_copy(flush_get_next_buf(lcd), px_map, &dirty_area);
                    flush_get_next_buf(lcd);
                }
            }
        }
    }

    lv_display_flush_ready(disp);
}

#else

static void flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LCD *lcd = (LCD *)lv_display_get_user_data(disp);

    /* Action after last area refresh */
    if (lv_display_flush_is_last(disp)) {
        /* Switch the current LCD frame buffer to `px_map` */
        lcd->switchFrameBufferTo(px_map);

        /* Waiting for the last frame buffer to complete transmission */
        ulTaskNotifyValueClear(NULL, ULONG_MAX);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    lv_display_flush_ready(disp);
}
#endif /* LVGL_PORT_ROTATION_DEGREE */

#elif LVGL_PORT_FULL_REFRESH && LVGL_PORT_DISP_BUFFER_NUM == 2

static void flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LCD *lcd = (LCD *)lv_display_get_user_data(disp);

    /* Switch the current LCD frame buffer to `px_map` */
    lcd->switchFrameBufferTo(px_map);

    /* Waiting for the last frame buffer to complete transmission */
    ulTaskNotifyValueClear(NULL, ULONG_MAX);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    lv_display_flush_ready(disp);
}

#elif LVGL_PORT_FULL_REFRESH && LVGL_PORT_DISP_BUFFER_NUM == 3

#if LVGL_PORT_ROTATION_DEGREE == 0
static void *lvgl_port_lcd_last_buf = NULL;
static void *lvgl_port_lcd_next_buf = NULL;
static void *lvgl_port_flush_next_buf = NULL;
#endif

static void flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LCD *lcd = (LCD *)lv_display_get_user_data(disp);

#if LVGL_PORT_ROTATION_DEGREE != 0
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;
    void *next_fb = get_next_frame_buffer(lcd);

    /* Rotate and copy dirty area from the current LVGL's buffer to the next LCD frame buffer */
    rotate_copy_pixel(
        (uint8_t *)px_map, (uint8_t *)next_fb, offsetx1, offsety1, offsetx2, offsety2, LV_HOR_RES,
        LV_VER_RES, LVGL_PORT_ROTATION_DEGREE
    );

    /* Switch the current LCD frame buffer to `next_fb` */
    lcd->switchFrameBufferTo(next_fb);
#else
    /*
     * v8 rewrote `draw_buf->buf1/buf2`. In v9 the active buffer is an `lv_draw_buf_t` and LVGL swaps
     * `buf_act` between `buf_1` and `buf_2` right after this callback, so point the *other* draw buffer
     * at the LCD frame buffer that is free for the next render.
     */
    lv_draw_buf_t *next_draw_buf = (disp->buf_act == disp->buf_1) ? disp->buf_2 : disp->buf_1;
    next_draw_buf->data = (uint8_t *)lvgl_port_flush_next_buf;
    next_draw_buf->unaligned_data = lvgl_port_flush_next_buf;
    lvgl_port_flush_next_buf = px_map;

    /* Switch the current LCD frame buffer to `px_map` */
    lcd->switchFrameBufferTo(px_map);

    lvgl_port_lcd_next_buf = px_map;
#endif

    lv_display_flush_ready(disp);
}
#endif

IRAM_ATTR bool onLcdVsyncCallback(void *user_data)
{
    BaseType_t need_yield = pdFALSE;
#if LVGL_PORT_FULL_REFRESH && (LVGL_PORT_DISP_BUFFER_NUM == 3) && (LVGL_PORT_ROTATION_DEGREE == 0)
    if (lvgl_port_lcd_next_buf != lvgl_port_lcd_last_buf) {
        lvgl_port_flush_next_buf = lvgl_port_lcd_last_buf;
        lvgl_port_lcd_last_buf = lvgl_port_lcd_next_buf;
    }
#else
    TaskHandle_t task_handle = (TaskHandle_t)user_data;
    // Notify that the current LCD frame buffer has been transmitted
    xTaskNotifyFromISR(task_handle, ULONG_MAX, eNoAction, &need_yield);
#endif
    return (need_yield == pdTRUE);
}

#else /* !LVGL_PORT_AVOID_TEAR */

/* Initial panel transformation, recorded once before any rotation is applied */
static bool disp_init_mirror_x = false;
static bool disp_init_mirror_y = false;
static bool disp_init_swap_xy = false;
/* True when the panel supports swap/mirror, so rotation is done by the LCD controller (v8: drv_update_cb) */
static bool disp_hw_rotation = false;
/* Scratch buffer used only for software rotation (v8: drv->sw_rotate) */
static void *disp_rotate_buf = nullptr;
static uint32_t disp_rotate_buf_size = 0;

static void flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LCD *lcd = (LCD *)lv_display_get_user_data(disp);
    lv_area_t flush_area = *area;
    uint8_t *flush_map = px_map;

    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    if (!disp_hw_rotation && (rotation != LV_DISPLAY_ROTATION_0) && (disp_rotate_buf != nullptr)) {
        /* Software rotation: rotate the rendered area into the scratch buffer, then rotate the coordinates */
        lv_color_format_t cf = lv_display_get_color_format(disp);
        int32_t w = lv_area_get_width(area);
        int32_t h = lv_area_get_height(area);
        uint32_t src_stride = lv_draw_buf_width_to_stride(w, cf);
        uint32_t dest_stride = ((rotation == LV_DISPLAY_ROTATION_90) || (rotation == LV_DISPLAY_ROTATION_270)) ?
                               lv_draw_buf_width_to_stride(h, cf) : src_stride;

        lv_draw_sw_rotate(px_map, disp_rotate_buf, w, h, src_stride, dest_stride, rotation, cf);
        lv_display_rotate_area(disp, &flush_area);
        flush_map = (uint8_t *)disp_rotate_buf;
    }

    lcd->drawBitmap(
        flush_area.x1, flush_area.y1, lv_area_get_width(&flush_area), lv_area_get_height(&flush_area),
        (const uint8_t *)flush_map
    );
    // For RGB LCD, directly notify LVGL that the buffer is ready
    if (lcd->getBus()->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        lv_display_flush_ready(disp);
    }
}

/**
 * @brief Apply hardware rotation when `lv_display_set_rotation()` is called (v8: `drv_update_cb`)
 */
static void resolution_changed_event_cb(lv_event_t *e)
{
    lv_display_t *disp = (lv_display_t *)lv_event_get_current_target(e);
    LCD *lcd = (LCD *)lv_event_get_user_data(e);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);

    switch (rotation) {
    case LV_DISPLAY_ROTATION_0:
        lcd->swapXY(disp_init_swap_xy);
        lcd->mirrorX(disp_init_mirror_x);
        lcd->mirrorY(disp_init_mirror_y);
        break;
    case LV_DISPLAY_ROTATION_90:
        lcd->swapXY(!disp_init_swap_xy);
        lcd->mirrorX(disp_init_mirror_x);
        lcd->mirrorY(!disp_init_mirror_y);
        break;
    case LV_DISPLAY_ROTATION_180:
        lcd->swapXY(disp_init_swap_xy);
        lcd->mirrorX(!disp_init_mirror_x);
        lcd->mirrorY(!disp_init_mirror_y);
        break;
    case LV_DISPLAY_ROTATION_270:
        lcd->swapXY(!disp_init_swap_xy);
        lcd->mirrorX(!disp_init_mirror_x);
        lcd->mirrorY(disp_init_mirror_y);
        break;
    }

    ESP_UTILS_LOGD("Update display rotation to %d", (int)rotation);

#if ESP_UTILS_CONF_LOG_LEVEL == ESP_UTILS_LOG_LEVEL_DEBUG
    auto transformation = lcd->getTransformation();
    ESP_UTILS_LOGD(
        "Current mirror x: %d, mirror y: %d, swap xy: %d", transformation.mirror_x, transformation.mirror_y,
        transformation.swap_xy
    );
#endif
}

#endif /* LVGL_PORT_AVOID_TEAR */

/**
 * @brief Align invalidated areas to the panel's coordinate alignment (v8: `rounder_cb`)
 */
static void rounder_event_cb(lv_event_t *e)
{
    LCD *lcd = (LCD *)lv_event_get_user_data(e);
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    uint8_t x_align = lcd->getBasicAttributes().basic_bus_spec.x_coord_align;
    uint8_t y_align = lcd->getBasicAttributes().basic_bus_spec.y_coord_align;

    if (x_align > 1) {
        // round the start of coordinate down to the nearest aligned value
        area->x1 &= ~(x_align - 1);
        // round the end of coordinate up to the nearest aligned value
        area->x2 = (area->x2 & ~(x_align - 1)) + x_align - 1;
    }

    if (y_align > 1) {
        // round the start of coordinate down to the nearest aligned value
        area->y1 &= ~(y_align - 1);
        // round the end of coordinate up to the nearest aligned value
        area->y2 = (area->y2 & ~(y_align - 1)) + y_align - 1;
    }
}

static lv_display_t *display_init(LCD *lcd)
{
    ESP_UTILS_CHECK_FALSE_RETURN(lcd != nullptr, nullptr, "Invalid LCD device");
    ESP_UTILS_CHECK_FALSE_RETURN(lcd->getRefreshPanelHandle() != nullptr, nullptr, "LCD device is not initialized");

    // Alloc draw buffers used by LVGL
    auto lcd_width = lcd->getFrameWidth();
    auto lcd_height = lcd->getFrameHeight();
    uint32_t buffer_size_px = 0;

    ESP_UTILS_LOGD("Malloc memory for LVGL buffer");
#if !LVGL_PORT_AVOID_TEAR
    // Avoid tearing function is disabled
    buffer_size_px = lcd_width * LVGL_PORT_BUFFER_SIZE_HEIGHT;
    for (int i = 0; (i < LVGL_PORT_BUFFER_NUM) && (i < LVGL_PORT_BUFFER_NUM_MAX); i++) {
        lvgl_buf[i] = heap_caps_aligned_alloc(
                          LV_DRAW_BUF_ALIGN, buffer_size_px * LVGL_PORT_PIXEL_SIZE, LVGL_PORT_BUFFER_MALLOC_CAPS
                      );
        assert(lvgl_buf[i]);
        ESP_UTILS_LOGD("Buffer[%d] address: %p, size: %d", i, lvgl_buf[i], (int)(buffer_size_px * LVGL_PORT_PIXEL_SIZE));
    }
#else
    // To avoid the tearing effect, we should use at least two frame buffers: one for LVGL rendering and another for LCD refresh
    buffer_size_px = lcd_width * lcd_height;
#if (LVGL_PORT_DISP_BUFFER_NUM >= 3) && (LVGL_PORT_ROTATION_DEGREE == 0) && LVGL_PORT_FULL_REFRESH

    // With the usage of three buffers and full-refresh, we always have one buffer available for rendering,
    // eliminating the need to wait for the LCD's sync signal
    lvgl_port_lcd_last_buf = lcd->getFrameBufferByIndex(0);
    lvgl_buf[0] = lcd->getFrameBufferByIndex(1);
    lvgl_buf[1] = lcd->getFrameBufferByIndex(2);
    lvgl_port_lcd_next_buf = lvgl_port_lcd_last_buf;
    lvgl_port_flush_next_buf = lvgl_buf[1];

#elif (LVGL_PORT_DISP_BUFFER_NUM >= 3) && (LVGL_PORT_ROTATION_DEGREE != 0)

    lvgl_buf[0] = lcd->getFrameBufferByIndex(2);

#elif LVGL_PORT_DISP_BUFFER_NUM >= 2

    for (int i = 0; (i < LVGL_PORT_DISP_BUFFER_NUM) && (i < LVGL_PORT_BUFFER_NUM_MAX); i++) {
        lvgl_buf[i] = lcd->getFrameBufferByIndex(i);
    }

#endif
#endif /* LVGL_PORT_AVOID_TEAR */

    ESP_UTILS_LOGD("Create LVGL display");
#if (LVGL_PORT_ROTATION_DEGREE == 90) || (LVGL_PORT_ROTATION_DEGREE == 270)
    lv_display_t *disp = lv_display_create(lcd_height, lcd_width);
#else
    lv_display_t *disp = lv_display_create(lcd_width, lcd_height);
#endif
    ESP_UTILS_CHECK_NULL_RETURN(disp, nullptr, "Create LVGL display failed");

    lv_display_set_color_format(disp, (LV_COLOR_DEPTH == 16) ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_NATIVE);
    lv_display_set_user_data(disp, (void *)lcd);
    lv_display_set_flush_cb(disp, flush_callback);

    // initialize LVGL draw buffers (v9 takes the size in BYTES)
#if LVGL_PORT_AVOID_TEAR    // Only available when the tearing effect is enabled
#if LVGL_PORT_FULL_REFRESH
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_FULL;
#else
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_DIRECT;
#endif
#else
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
#endif
    lv_display_set_buffers(disp, lvgl_buf[0], lvgl_buf[1], buffer_size_px * LVGL_PORT_PIXEL_SIZE, render_mode);

#if !LVGL_PORT_AVOID_TEAR   // Only available when the tearing effect is disabled
    auto transformation = lcd->getTransformation();
    disp_init_mirror_x = transformation.mirror_x;
    disp_init_mirror_y = transformation.mirror_y;
    disp_init_swap_xy = transformation.swap_xy;

    if (lcd->getBasicAttributes().basic_bus_spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_SWAP_XY) &&
            lcd->getBasicAttributes().basic_bus_spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_MIRROR_X) &&
            lcd->getBasicAttributes().basic_bus_spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_MIRROR_Y)) {
        // The LCD controller can rotate by itself
        disp_hw_rotation = true;
        lv_display_add_event_cb(disp, resolution_changed_event_cb, LV_EVENT_RESOLUTION_CHANGED, (void *)lcd);
    } else {
        // Software rotation: needs a scratch buffer as big as one draw buffer
        disp_hw_rotation = false;
        disp_rotate_buf_size = buffer_size_px * LVGL_PORT_PIXEL_SIZE;
        disp_rotate_buf = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, disp_rotate_buf_size, LVGL_PORT_BUFFER_MALLOC_CAPS);
        if (disp_rotate_buf == nullptr) {
            ESP_UTILS_LOGW("No memory for the software rotation buffer, `lv_display_set_rotation()` will be ignored");
        }
    }
#endif /* LVGL_PORT_AVOID_TEAR */

    // Only available when the coordinate alignment is enabled
    if ((lcd->getBasicAttributes().basic_bus_spec.x_coord_align > 1) ||
            (lcd->getBasicAttributes().basic_bus_spec.y_coord_align > 1)) {
        lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, (void *)lcd);
    }

    return disp;
}

static SemaphoreHandle_t touch_detected;

static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    Touch *tp = (Touch *)lv_indev_get_user_data(indev);
    TouchPoint point;
    data->state = LV_INDEV_STATE_RELEASED;

    /* if we are interrupt driven wait for the ISR to fire */
    if ( tp->isInterruptEnabled() && (xSemaphoreTake( touch_detected, 0 ) == pdFALSE) ) {
        return;
    }

    /* Read data from touch controller */
    int read_touch_result = tp->readPoints(&point, 1, 0);
    if (read_touch_result > 0) {
        // LVGL v9 rotates pointer coordinates by itself according to `lv_display_set_rotation()`
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

static bool onTouchInterruptCallback(void *user_data)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR( touch_detected, &xHigherPriorityTaskWoken );
    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
    return false;
}

static lv_indev_t *indev_init(lv_display_t *disp, Touch *tp)
{
    ESP_UTILS_CHECK_FALSE_RETURN(tp != nullptr, nullptr, "Invalid touch device");
    ESP_UTILS_CHECK_FALSE_RETURN(tp->getPanelHandle() != nullptr, nullptr, "Touch device is not initialized");

    if (tp->isInterruptEnabled()) {
        touch_detected = xSemaphoreCreateBinary();
        tp->attachInterruptCallback(onTouchInterruptCallback, tp);
    }
    ESP_UTILS_LOGD("Register input device to LVGL");
    lv_indev_t *indev = lv_indev_create();
    ESP_UTILS_CHECK_NULL_RETURN(indev, nullptr, "Create LVGL input device failed");
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchpad_read);
    lv_indev_set_user_data(indev, (void *)tp);
    lv_indev_set_display(indev, disp);

    return indev;
}

/**
 * @brief LVGL v9 tick source (replaces the v8 periodic `lv_tick_inc()` esp_timer)
 */
static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void lvgl_port_task(void *arg)
{
    ESP_UTILS_LOGD("Starting LVGL task");

    uint32_t task_delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
    while (1) {
        if (lvgl_port_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (task_delay_ms > LVGL_PORT_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_PORT_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_PORT_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

IRAM_ATTR bool onDrawBitmapFinishCallback(void *user_data)
{
    lv_display_t *disp = (lv_display_t *)user_data;

    lv_display_flush_ready(disp);

    return false;
}

bool lvgl_port_init(LCD *lcd, Touch *tp)
{
    ESP_UTILS_CHECK_FALSE_RETURN(lcd != nullptr, false, "Invalid LCD device");

    auto bus_type = lcd->getBus()->getBasicAttributes().type;
#if LVGL_PORT_AVOID_TEAR
    ESP_UTILS_CHECK_FALSE_RETURN(
        (bus_type == ESP_PANEL_BUS_TYPE_RGB) || (bus_type == ESP_PANEL_BUS_TYPE_MIPI_DSI), false,
        "Avoid tearing function only works with RGB/MIPI-DSI LCD now"
    );
    ESP_UTILS_LOGI(
        "Avoid tearing is enabled, mode: %d, rotation: %d", LVGL_PORT_AVOID_TEARING_MODE, LVGL_PORT_ROTATION_DEGREE
    );
#endif

    lv_init();
    lv_tick_set_cb(tick_get_cb);

    ESP_UTILS_LOGI("Initializing LVGL display driver");
    lvgl_disp = display_init(lcd);
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_disp, false, "Initialize LVGL display driver failed");
    // Record the initial rotation of the display
    lv_display_set_rotation(lvgl_disp, LV_DISPLAY_ROTATION_0);

    // For non-RGB LCD, need to notify LVGL that the buffer is ready when the refresh is finished
    if (bus_type != ESP_PANEL_BUS_TYPE_RGB) {
        ESP_UTILS_LOGD("Attach refresh finish callback to LCD");
        lcd->attachDrawBitmapFinishCallback(onDrawBitmapFinishCallback, (void *)lvgl_disp);
    }

    if (tp != nullptr) {
        ESP_UTILS_LOGD("Initialize LVGL input driver");
        lvgl_indev = indev_init(lvgl_disp, tp);
        ESP_UTILS_CHECK_NULL_RETURN(lvgl_indev, false, "Initialize LVGL input driver failed");

#if LVGL_PORT_ROTATION_DEGREE != 0
        auto &transformation = tp->getTransformation();
#if LVGL_PORT_ROTATION_DEGREE == 90
        tp->swapXY(!transformation.swap_xy);
        tp->mirrorY(!transformation.mirror_y);
#elif LVGL_PORT_ROTATION_DEGREE == 180
        tp->mirrorX(!transformation.mirror_x);
        tp->mirrorY(!transformation.mirror_y);
#elif LVGL_PORT_ROTATION_DEGREE == 270
        tp->swapXY(!transformation.swap_xy);
        tp->mirrorX(!transformation.mirror_x);
#endif
#endif
    }

    ESP_UTILS_LOGD("Create mutex for LVGL");
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "Create LVGL mutex failed");

    ESP_UTILS_LOGD("Create LVGL task");
    BaseType_t core_id = (LVGL_PORT_TASK_CORE < 0) ? tskNO_AFFINITY : LVGL_PORT_TASK_CORE;
    BaseType_t ret = xTaskCreatePinnedToCore(lvgl_port_task, "lvgl", LVGL_PORT_TASK_STACK_SIZE, NULL,
                     LVGL_PORT_TASK_PRIORITY, &lvgl_task_handle, core_id);
    ESP_UTILS_CHECK_FALSE_RETURN(ret == pdPASS, false, "Create LVGL task failed");

#if LVGL_PORT_AVOID_TEAR
    lcd->attachRefreshFinishCallback(onLcdVsyncCallback, (void *)lvgl_task_handle);
#endif

    return true;
}

bool lvgl_port_lock(int timeout_ms)
{
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "LVGL mutex is not initialized");

    const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE);
}

bool lvgl_port_unlock(void)
{
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "LVGL mutex is not initialized");

    xSemaphoreGiveRecursive(lvgl_mux);

    return true;
}

bool lvgl_port_deinit(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(lvgl_port_lock(-1), false, "Lock LVGL failed");
    if (lvgl_task_handle != nullptr) {
        vTaskDelete(lvgl_task_handle);
        lvgl_task_handle = nullptr;
    }
    ESP_UTILS_CHECK_FALSE_RETURN(lvgl_port_unlock(), false, "Unlock LVGL failed");

    if (lvgl_indev != nullptr) {
        lv_indev_delete(lvgl_indev);
        lvgl_indev = nullptr;
    }
    if (lvgl_disp != nullptr) {
        lv_display_delete(lvgl_disp);
        lvgl_disp = nullptr;
    }
    lv_deinit();

#if !LVGL_PORT_AVOID_TEAR
    for (int i = 0; i < LVGL_PORT_BUFFER_NUM; i++) {
        if (lvgl_buf[i] != nullptr) {
            heap_caps_free(lvgl_buf[i]);
            lvgl_buf[i] = nullptr;
        }
    }
    if (disp_rotate_buf != nullptr) {
        heap_caps_free(disp_rotate_buf);
        disp_rotate_buf = nullptr;
        disp_rotate_buf_size = 0;
    }
#endif
    if (lvgl_mux != nullptr) {
        vSemaphoreDelete(lvgl_mux);
        lvgl_mux = nullptr;
    }

    return true;
}
