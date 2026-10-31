#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "usage_provider.h"

/* A small, independently mounted Home widget. The parent owns its LVGL objects. */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *setup;
    lv_obj_t *title;
    lv_obj_t *state;
    lv_obj_t *tokens;
    lv_obj_t *divider;
    struct {
        lv_obj_t *name;
        lv_obj_t *value;
        lv_obj_t *bar;
        lv_obj_t *reset;
    } window[2];
    int16_t bar_width[2];
    bool weekly_only;
} usage_app_widget_t;

void usage_app_widget_mount(usage_app_widget_t *widget, lv_obj_t *parent,
                            int x, int y, int width, int height,
                            usage_provider_id_t provider, const char *name);
void usage_app_widget_update(usage_app_widget_t *widget,
                             const provider_usage_t *usage, bool enabled,
                             bool clock_ready, int64_t now_epoch,
                             int64_t now_us);
