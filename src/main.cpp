#include "main.h"
#include "lottie/lottie_demo.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup()
{
    Serial.begin(115200);

    Serial.println("Initializing board");
    Board *board = new Board();
    board->init();

    assert(board->begin());

    Serial.println("Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    Serial.println("Creating UI");

    // Lock the FreeRTOS mutex before modifying the LVGL UI
    lvgl_port_lock(-1);

    // Initialize the EEZ studio generated screens
    ui_init();

    //Create a lottie animation on the main screen
    lottie_demo_create(objects.main);

    // You may need to adjust the rotation depending on the Viewe board's default orientation
    // For standard 320x240 landscape, you typically want LV_DISPLAY_ROTATION_0 or LV_DISPLAY_ROTATION_90
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);

    lvgl_port_unlock();

    Serial.println("UI loaded");

}

void loop()
{
    // Lock the LVGL port safely
    lvgl_port_lock(-1);

    // Call the EEZ studio tick function to process screen states
    ui_tick();

    // NOTE: `lv_timer_handler()` is NOT called here. The LVGL port already runs it in its own
    // FreeRTOS task ("lvgl", see lvgl_v9_port.cpp); calling it from loop() as well made two tasks
    // render/flush concurrently (serialised by the mutex, but wasteful and it breaks the
    // anti-tearing modes, which wait for a notification addressed to the LVGL task).

    lvgl_port_unlock();

    static uint32_t last_report = 0;
    if (millis() - last_report > 3000) {
        last_report = millis();
        lottie_demo_report();
    }

    // Small delay to yield to FreeRTOS
    delay(5);
}

