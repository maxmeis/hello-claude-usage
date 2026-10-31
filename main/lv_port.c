/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from sensecap-sdk/examples/squareline_demo/main/lv_port.c.
 */
#include <assert.h>
#include <string.h>

#include "bsp_board.h"
#include "bsp_lcd.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "indev/indev.h"
#include "lvgl.h"
#include "lv_port.h"
#include "sdkconfig.h"

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#endif

#if CONFIG_LCD_AVOID_TEAR || CONFIG_LCD_LVGL_DIRECT_MODE
#include "esp_cache.h"
#endif

#ifndef CONFIG_LCD_TASK_PRIORITY
#define CONFIG_LCD_TASK_PRIORITY 5
#endif

static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static SemaphoreHandle_t s_lvgl_mutex;
static TaskHandle_t s_lvgl_task;

void lv_port_set_brightness(uint8_t percent)
{
#ifdef ESP_PLATFORM
    const board_res_desc_t *board = bsp_board_get_description();
    if (board->GPIO_LCD_BL == GPIO_NUM_NC) return;
    if (percent < 20 || percent > 100) percent = 100;
    if (percent == 100) {
        ESP_ERROR_CHECK(bsp_lcd_set_backlight(true));
        return;
    }

    /* The SenseCAP BSP initializes this pin as a GPIO. A separate LEDC
     * timer/channel avoids the BSP's optional RGB LED channels 0..2. */
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_3,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    uint32_t duty = (1023u * (board->GPIO_LCD_BL_ON ? percent : 100u - percent) + 50u) / 100u;
    const ledc_channel_config_t channel = {
        .gpio_num = board->GPIO_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_7,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_3,
        .duty = duty,
        .hpoint = 0,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK) err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGW("lv_port", "Backlight PWM unavailable (%s); using full brightness",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(bsp_lcd_set_backlight(true));
    }
#else
    (void)percent;
#endif
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static lv_point_t last_valid_point = {0, 0};
    const board_res_desc_t *board = bsp_board_get_description();
    indev_data_t touch = {
        .x = last_valid_point.x,
        .y = last_valid_point.y,
        .pressed = false,
    };

    /* Start released so SDK read failures cannot leave LVGL stuck pressed. */
    data->state = LV_INDEV_STATE_RELEASED;
    data->point = last_valid_point;

    if (indev_get_major_value(&touch) != ESP_OK || !touch.pressed ||
        touch.x < 0 || touch.y < 0 ||
        touch.x >= board->LCD_WIDTH || touch.y >= board->LCD_HEIGHT) {
        return;
    }

    /* The SquareLine board port flips both axes after the BSP rotation transform. */
    last_valid_point.x = board->LCD_WIDTH - 1 - touch.x;
    last_valid_point.y = board->LCD_HEIGHT - 1 - touch.y;
    data->point = last_valid_point;
    data->state = LV_INDEV_STATE_PRESSED;
}

static bool flush_ready(void *unused)
{
    (void)unused;
    lv_disp_flush_ready(&s_disp_drv);
    return false;
}

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
{
    (void)drv;
    ESP_ERROR_CHECK(bsp_lcd_flush(area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels));
}

#if CONFIG_LCD_LVGL_DIRECT_MODE
static bool flush_is_last(void)
{
    return lv_disp_flush_is_last(&s_disp_drv);
}

/* Keep both RGB frame buffers identical after LVGL draws only dirty regions. */
static void direct_mode_copy(void)
{
    lv_disp_t *disp = _lv_refr_get_disp_refreshing();
    uint8_t *active = disp->driver->draw_buf->buf_act;
    uint8_t *other = active == disp->driver->draw_buf->buf1
                     ? disp->driver->draw_buf->buf2 : disp->driver->draw_buf->buf1;
    const size_t stride = disp->driver->hor_res * sizeof(lv_color_t);

    for (int i = 0; i < disp->inv_p; ++i) {
        if (disp->inv_area_joined[i]) {
            continue;
        }
        const lv_area_t *area = &disp->inv_areas[i];
        const size_t x_offset = area->x1 * sizeof(lv_color_t);
        const size_t width = (area->x2 - area->x1 + 1) * sizeof(lv_color_t);
        for (int y = area->y1; y <= area->y2; ++y) {
            memcpy(other + y * stride + x_offset, active + y * stride + x_offset, width);
        }
        ESP_ERROR_CHECK(esp_cache_msync(other + area->y1 * stride,
                                       (area->y2 - area->y1 + 1) * stride, 0));
    }
}
#endif

static void tick_cb(void *arg)
{
    lv_tick_inc(*(const uint32_t *)arg);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
        lv_timer_handler();
        xSemaphoreGive(s_lvgl_mutex);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void lv_port_init(void)
{
    static lv_disp_draw_buf_t draw_buf;
    static const uint32_t tick_ms = 2;
    const board_res_desc_t *board = bsp_board_get_description();
    void *buf1 = NULL;
    void *buf2 = NULL;
    const uint32_t pixels = board->LCD_WIDTH * board->LCD_HEIGHT;

    lv_init();
#if CONFIG_LCD_AVOID_TEAR
    bsp_lcd_get_frame_buffer(&buf1, &buf2);
    assert(buf1 && buf2);
    /* The SDK initially paints only its first RGB buffer blue (0x00ff).
     * Start both direct-mode buffers with the same black pixels so an
     * untouched region cannot retain that blue after a buffer swap. */
    memset(buf1, 0, pixels * sizeof(lv_color_t));
    memset(buf2, 0, pixels * sizeof(lv_color_t));
    ESP_ERROR_CHECK(esp_cache_msync(buf1, pixels * sizeof(lv_color_t), 0));
    ESP_ERROR_CHECK(esp_cache_msync(buf2, pixels * sizeof(lv_color_t), 0));
#else
    buf1 = heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1);
#endif
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, pixels);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = board->LCD_WIDTH;
    s_disp_drv.ver_res = board->LCD_HEIGHT;
    s_disp_drv.flush_cb = disp_flush;
    s_disp_drv.draw_buf = &draw_buf;
#if CONFIG_LCD_LVGL_FULL_REFRESH
    s_disp_drv.full_refresh = 1;
#elif CONFIG_LCD_LVGL_DIRECT_MODE
    s_disp_drv.direct_mode = 1;
#endif
    ESP_ERROR_CHECK(bsp_lcd_set_cb(flush_ready, NULL));
#if CONFIG_LCD_LVGL_DIRECT_MODE
    bsp_lcd_flush_is_last_register(flush_is_last);
    bsp_lcd_direct_mode_register(direct_mode_copy);
#endif
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read;
    lv_indev_drv_register(&s_indev_drv);

    const esp_timer_create_args_t timer_args = {
        .callback = tick_cb,
        .arg = (void *)&tick_ms,
        .name = "lvgl_tick",
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, tick_ms * 1000));

    s_lvgl_mutex = xSemaphoreCreateMutex();
    assert(s_lvgl_mutex);
    BaseType_t created = xTaskCreate(lvgl_task, "lvgl_task", 4096 * 4, NULL,
                                     CONFIG_LCD_TASK_PRIORITY, &s_lvgl_task);
    assert(created == pdPASS);
}

void lv_port_sem_take(void)
{
    if (xTaskGetCurrentTaskHandle() != s_lvgl_task) {
        xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    }
}

void lv_port_sem_give(void)
{
    if (xTaskGetCurrentTaskHandle() != s_lvgl_task) {
        xSemaphoreGive(s_lvgl_mutex);
    }
}
