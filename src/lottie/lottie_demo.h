#pragma once
#include "lvgl.h"

/* Creates a Lottie animation on `parent`. Call with the LVGL lock held. */
lv_obj_t *lottie_demo_create(lv_obj_t *parent);

/* Prints heap, PSRAM and LVGL task stack headroom to Serial. Safe from loop(). */
void lottie_demo_report(void);
