#include "display.h"
#include "display_format.h"
#include "ui_shell.h"
#include "usage_app_ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp_board.h"
#include "box_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lv_port.h"
#include "lvgl.h"
#include "usage_fonts.h"
#include "usage_provider.h"
#include "word_clock.h"

#define BG 0x000000
#define CARD 0x252B27
#define TEXT 0xF9F6EF
#define MUTED 0xDDDCD6
#define META 0xA6A49F
#define ORANGE 0xD97757
#define WEEK 0xB3C84D
#define FORECAST_GREEN 0x89B18B
#define FORECAST_NEUTRAL 0x777570
#define W 432
#define CARD_H 132
#define CARD_GAP 18
#define CARD_PAD 16
#define BAR_W (W - 2 * CARD_PAD)
#define STALE_SEC 900
#define FACE_MS 5000
#define FACE_FADE_IN_MS 300
#define FACE_FADE_OUT_MS 350
#define FACE_TICK_MS 20
#define FACE_INK 0x151A17
#define FACE_EYE_W 48
#define FACE_EYE_H 128
#define MASCOT_X 60
#define MASCOT_Y 0
#define MASCOT_TICK_MS 100
#define MASCOT_CYCLE_MS 4000
#define MASCOT_PLAY_START_MS 1200
#define WORD_X 68
#define WORD_Y 80
#define WORD_CELL 40
#define WORD_COL_SPAN 304
#define WORD_ROW_SPAN 298
#define WORD_INACTIVE 0x181818


typedef enum { VIEW_STATUS, VIEW_HOME, VIEW_APP, VIEW_CLOCK } rendered_view_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *period;
    lv_obj_t *value;
    lv_obj_t *fill;
    lv_obj_t *reset;
    lv_obj_t *forecast_headroom;
    lv_obj_t *forecast_tight;
    lv_obj_t *forecast_unknown;
    lv_obj_t *forecast_root;
    lv_obj_t *reset_icon;
    float utilized;
    int64_t sampled_at;
    int64_t window_seconds;
    char reset_iso[64];
} card_t;

static lv_obj_t *s_content;
static lv_obj_t *s_home_layer;
static lv_obj_t *s_home_hitbox, *s_home_label;
static lv_obj_t *s_header_refresh_hitbox, *s_header_refresh_label;
static lv_obj_t *s_header_settings_hitbox, *s_header_settings_label;
static lv_obj_t *s_mascot, *s_mascot_left_arm, *s_mascot_right_arm;
static lv_obj_t *s_mascot_left_foot, *s_mascot_right_foot;
static lv_obj_t *s_mascot_hitbox;
static lv_obj_t *s_heading;
static lv_obj_t *s_provider_label;
static lv_obj_t *s_credits;
static lv_obj_t *s_word_layer, *s_word_hitbox, *s_word_wait;
static lv_obj_t *s_word_letters[WORD_CLOCK_CELLS];
static lv_obj_t *s_word_dots[4];
static lv_obj_t *s_footer;
static lv_obj_t *s_updated_age;
static lv_obj_t *s_refresh_hitbox;
static lv_obj_t *s_settings_hitbox, *s_settings_label;
static lv_obj_t *s_settings_overlay, *s_settings_title;
static lv_obj_t *s_settings_detail, *s_settings_qr_field, *s_settings_qr;
static lv_obj_t *s_settings_name;
static lv_obj_t *s_wifi_overlay, *s_wifi_title, *s_wifi_detail;
static lv_obj_t *s_wifi_qr_field, *s_wifi_qr;
static lv_obj_t *s_wifi_name, *s_setup_name;
static lv_obj_t *s_wifi_step1, *s_wifi_step2, *s_wifi_done;
static lv_obj_t *s_wifi_step1_mark, *s_wifi_step2_mark, *s_wifi_scan_hint;
static char s_device_name[sizeof("hello-box-0000")];
static lv_obj_t *s_face, *s_face_left_eye, *s_face_right_eye, *s_boot_label;
static lv_timer_t *s_face_timer;
static card_t s_five, s_week;
static provider_usage_t s_provider_data[USAGE_PROVIDER_COUNT];
typedef struct {
    usage_provider_id_t provider;
    usage_app_widget_t widget;
} display_app_context_t;
static display_app_context_t s_app_contexts[USAGE_PROVIDER_COUNT] = {
    {.provider = USAGE_PROVIDER_CLAUDE}, {.provider = USAGE_PROVIDER_CODEX},
};
static app_registry_t s_apps;
static screensaver_registry_t s_savers;
static ui_shell_t s_shell;
static app_id_t s_detail_app;
static bool s_clock_ready, s_has_usage, s_has_sample, s_updating, s_setup_visible;
static bool s_time_valid;
static bool s_face_active, s_refresh_pending, s_cycle_inflight;
static uint32_t s_refresh_pending_mask;
static uint32_t s_escaped_pending_mask;
static bool s_boot_active, s_boot_ready;
static bool s_settings_active, s_settings_accepted, s_wifi_setup_active;
static bool s_web_setup_active;
static bool settings_overlay_open(void)
{
    return s_settings_active || s_web_setup_active;
}
static uint32_t s_settings_sequence, s_settings_request_id;
static display_settings_callback_t s_settings_callback;
static void *s_settings_context;
static int64_t s_face_started_us;
static int64_t s_boot_fade_started_us;
static int64_t s_mascot_cycle_started_us;
static int8_t s_mascot_frame = -1;
static int16_t s_mascot_base_x = MASCOT_X;
static display_refresh_callback_t s_refresh_callback;
static void *s_refresh_context;
static display_app_refresh_callback_t s_app_refresh_callback;
static void *s_app_refresh_context;
static int64_t s_measured_us;
static bool s_unknown_usage;
static rendered_view_t s_rendered_view = VIEW_STATUS;
static int s_word_render_minute = -1;
static int s_word_render_hour = -1;
static bool s_word_render_ready;
static char s_error[72];
static const char *s_status_prefix = "Error";
static void refresh(lv_timer_t *timer);
static void clear_content(void);
static card_t create_card(int y, const char *period, uint32_t color);
static void update_card(card_t *card, float used, const char *reset);
static void format_window_name(char *out, size_t size, uint32_t seconds, int index);
static void reset_iso_from_epoch(char *out, size_t size, int64_t epoch);
static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       int x, int y, int width, const char *text);
static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int width, int height,
                      uint32_t color, int radius);
static int word_row_y(int row);
static void update_word_clock(int hour, int minute, bool ready);
static void render_shell_view(void);
static void mount_detail(app_id_t id);
static void update_detail(app_id_t id);
static void home_widget_clicked(lv_event_t *event);
static void settings_clicked(lv_event_t *event);
static void open_web_setup(usage_provider_id_t provider);
static void close_settings(bool notify_backend);
static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int width, int height,
                      uint32_t color, int radius);

static lv_obj_t *forecast_circle(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *circle = rect(parent, 3, 3, 10, 10, color, LV_RADIUS_CIRCLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    return circle;
}

static bool app_enabled(void *context)
{
    const display_app_context_t *app = context;
    return usage_provider_enabled(box_config_current()->providers, app->provider);
}

static bool app_configured(void *context)
{
    const display_app_context_t *app = context;
    return s_provider_data[app->provider].configured;
}

static bool app_request_refresh(void *context)
{
    const display_app_context_t *app = context;
    return s_app_refresh_callback &&
        s_app_refresh_callback(s_app_refresh_context, (app_id_t)app->provider + 1);
}

static void app_mount_widget(void *context, void *parent)
{
    display_app_context_t *app = context;
    lv_obj_t *slot = parent;
    usage_app_widget_mount(&app->widget, slot, 0, 0,
                            lv_obj_get_width(slot), lv_obj_get_height(slot),
                            app->provider, usage_provider_name(app->provider));
    lv_obj_add_event_cb(app->widget.root, home_widget_clicked, LV_EVENT_CLICKED, app);
}

static void mount_detail(app_id_t id)
{
    if (id != APP_ID_CLAUDE && id != APP_ID_CODEX) return;
    clear_content();
    s_detail_app = id;
    if (id == APP_ID_CODEX) {
        s_five = create_card(18, "Week left", WEEK);
        lv_obj_set_height(s_five.root, 282);
        lv_obj_set_pos(s_five.period, 16, 18);
        lv_obj_set_width(s_five.period, 380);
        lv_obj_set_style_text_align(s_five.period, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_font(s_five.period, &usage_status_font, 0);
        lv_obj_set_pos(s_five.value, 16, 45);
        lv_obj_set_width(s_five.value, 400);
        lv_obj_set_style_text_font(s_five.value, &usage_hero_font, 0);
        lv_obj_set_pos(s_five.forecast_root, 397, 20);
        lv_obj_set_y(s_five.fill, 164);
        lv_obj_set_y(s_five.reset_icon, 211);
        lv_obj_set_y(s_five.reset, 205);
        lv_obj_set_style_text_font(s_five.reset, &usage_status_font, 0);
        lv_obj_set_pos(s_credits, 24, 380);
        lv_obj_set_width(s_credits, W);
    } else {
        s_five = create_card(0, "5h left", ORANGE);
        s_week = create_card(CARD_H + CARD_GAP, "Week left", WEEK);
        lv_obj_set_pos(s_credits, 24, 390);
        lv_obj_set_width(s_credits, W);
    }
    update_detail(id);
}

static void app_mount_detail(void *context, void *parent)
{
    (void)parent;
    const display_app_context_t *app = context;
    mount_detail((app_id_t)app->provider + 1);
}

static void update_detail(app_id_t id)
{
    if (id != s_detail_app || !s_five.value) return;
    const provider_usage_t *data = &s_provider_data[id - 1];
    char period[24], reset[32];
    card_t *cards[2] = {&s_five, &s_week};
    int count = id == APP_ID_CODEX ? 1 : 2;
    const quota_window_t *weekly = id == APP_ID_CODEX ?
        usage_provider_weekly_window(data) : NULL;
    for (int i = 0; i < count; ++i) {
        const quota_window_t *window = id == APP_ID_CODEX ? weekly : &data->windows[i];
        if (!window) {
            lv_label_set_text(cards[i]->period, "Weekly usage unavailable");
            lv_obj_set_pos(cards[i]->period, 16, 105);
            lv_obj_set_width(cards[i]->period, 400);
            lv_obj_add_flag(cards[i]->value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cards[i]->forecast_root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cards[i]->reset_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cards[i]->reset, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(cards[i]->fill, 0);
            cards[i]->reset_iso[0] = 0;
            cards[i]->utilized = NAN;
            cards[i]->sampled_at = data->sampled_at;
            cards[i]->window_seconds = 604800;
            continue;
        }
        if (id == APP_ID_CODEX) {
            lv_label_set_text(cards[i]->period, "Week left");
            lv_obj_set_pos(cards[i]->period, 16, 18);
            lv_obj_clear_flag(cards[i]->value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cards[i]->forecast_root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cards[i]->reset_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cards[i]->reset, LV_OBJ_FLAG_HIDDEN);
        }
        if (id == APP_ID_CODEX) {
            lv_obj_set_style_text_font(cards[i]->value, &usage_hero_font, 0);
            lv_obj_set_y(cards[i]->value, 45);
        }
        format_window_name(period, sizeof(period), window->window_seconds, i);
        if (id != APP_ID_CODEX) lv_label_set_text(cards[i]->period, period);
        reset_iso_from_epoch(reset, sizeof(reset), window->resets_at);
        update_card(cards[i], data->configured && data->has_sample && window->valid
                                  ? window->used_percent : NAN, reset);
        cards[i]->utilized = data->configured && data->has_sample && window->valid
                                 ? window->used_percent : NAN;
        cards[i]->sampled_at = data->sampled_at;
        cards[i]->window_seconds = window->window_seconds;
    }
    char credits[40];
    ui_shell_view_t route = ui_shell_view(&s_shell);
    if (data->configured && data->reset_credits_known) {
        snprintf(credits, sizeof(credits), "Reset tokens: %lu",
                 (unsigned long)data->reset_credits);
        lv_label_set_text(s_credits, credits);
        if (route.route == UI_ROUTE_APP && route.app_id == id)
            lv_obj_clear_flag(s_credits, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_credits, "");
        lv_obj_add_flag(s_credits, LV_OBJ_FLAG_HIDDEN);
    }
    s_measured_us = data->sampled_us > 0 ? data->sampled_us : esp_timer_get_time();
    s_has_sample = data->has_sample;
    s_unknown_usage = !data->configured || !data->has_sample ||
                      (id == APP_ID_CODEX ? !weekly :
                       (!data->windows[0].valid && !data->windows[1].valid));
}

static void app_update_views(void *context)
{
    display_app_context_t *app = context;
    usage_app_widget_update(&app->widget, &s_provider_data[app->provider],
        app_enabled(app), s_time_valid, (int64_t)time(NULL), esp_timer_get_time());
    update_detail((app_id_t)app->provider + 1);
}

static void saver_mount(void *context, void *parent)
{
    (void)context;
    lv_obj_t *screen = parent;
    s_word_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_word_layer);
    lv_obj_set_size(s_word_layer, 480, 480);
    lv_obj_set_style_bg_opa(s_word_layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_word_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int row = 0; row < WORD_CLOCK_ROWS; row++) {
        const char *cursor = word_clock_rows_for_language(
            box_config_current()->clock_language)[row];
        size_t remaining = strlen(cursor);
        for (int col = 0; col < WORD_CLOCK_COLS; col++) {
            int index = row * WORD_CLOCK_COLS + col;
            char letter[5] = {0};
            if (remaining > 0) {
                size_t bytes = _lv_txt_encoded_size(cursor);
                if (bytes > 0 && bytes <= 4 && bytes <= remaining) {
                    memcpy(letter, cursor, bytes);
                    cursor += bytes;
                    remaining -= bytes;
                }
            }
            lv_obj_t *cell = label(s_word_layer, &usage_ui_font, WORD_INACTIVE,
                                   WORD_X + (col * WORD_COL_SPAN + WORD_CLOCK_COLS / 2) /
                                                (WORD_CLOCK_COLS - 1), word_row_y(row),
                                   WORD_CELL, letter);
            lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
            s_word_letters[index] = cell;
        }
    }
    static const lv_point_t dot_positions[4] = {
        {12, 12}, {458, 12}, {458, 458}, {12, 458},
    };
    for (int i = 0; i < 4; i++) {
        s_word_dots[i] = rect(s_word_layer, dot_positions[i].x, dot_positions[i].y,
                              10, 10, WORD_INACTIVE, LV_RADIUS_CIRCLE);
        lv_obj_clear_flag(s_word_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_word_wait = label(s_word_layer, &usage_ui_font, META, 350, 32, 106, "--:--");
    lv_obj_set_style_text_align(s_word_wait, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(s_word_layer, LV_OBJ_FLAG_HIDDEN);
}

static void saver_update(void *context)
{
    (void)context;
    time_t now = time(NULL);
    struct tm local;
    bool ready = s_clock_ready && localtime_r(&now, &local) && local.tm_year >= 125;
    update_word_clock(ready ? local.tm_hour : -1,
                      ready ? local.tm_min : -1, ready);
}

static void saver_visible(void *context, bool visible)
{
    (void)context;
    if (visible) {
        lv_obj_clear_flag(s_word_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_word_layer);
    } else lv_obj_add_flag(s_word_layer, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *icon_container(lv_obj_t *parent)
{
    lv_obj_t *shape = lv_obj_create(parent);
    lv_obj_remove_style_all(shape);
    lv_obj_set_pos(shape, 0, 0);
    lv_obj_set_size(shape, 16, 16);
    lv_obj_clear_flag(shape, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return shape;
}

static void update_forecast_icon(card_t *card, int64_t now, bool fresh,
                                 int64_t window_seconds)
{
    display_forecast_t forecast = display_forecast(card->utilized, card->reset_iso,
        window_seconds, card->sampled_at, now, fresh);
    lv_obj_t *icons[] = {card->forecast_unknown, card->forecast_headroom,
                         card->forecast_tight};
    display_forecast_t states[] = {DISPLAY_FORECAST_UNKNOWN,
                                   DISPLAY_FORECAST_HEADROOM,
                                   DISPLAY_FORECAST_TIGHT};
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
        if (states[i] == forecast) lv_obj_clear_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void reset_iso_from_epoch(char *out, size_t size, int64_t epoch)
{
    out[0] = '\0';
    if (epoch <= 0) return;
    time_t value = (time_t)epoch;
    if ((int64_t)value != epoch) return;
    struct tm utc;
    if (gmtime_r(&value, &utc))
        strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static void format_window_name(char *out, size_t size, uint32_t seconds, int index)
{
    if (seconds == 0) snprintf(out, size, "No data");
    else if (seconds == 18000) snprintf(out, size, "5h left");
    else if (seconds == 604800) snprintf(out, size, "Week left");
    else if (seconds >= 86400 && seconds % 86400 == 0)
        snprintf(out, size, "%lud left", (unsigned long)(seconds / 86400));
    else if (seconds >= 3600 && seconds % 3600 == 0)
        snprintf(out, size, "%luh left", (unsigned long)(seconds / 3600));
    else snprintf(out, size, "Window %d", index + 1);
}

static void update_reset_label(lv_obj_t *target, const char *iso, int64_t now, bool ready)
{
    char text[64];
    display_format_reset(text, sizeof(text), iso, now, ready);
    const char *compact = text;
    if (strncmp(text, "Resets in ", 10) == 0) compact = text + 10;
    else if (strcmp(text, "Reset unknown") == 0) compact = "Unknown";
    else if (strcmp(text, "Awaiting reset") == 0) compact = "Pending";
    lv_label_set_text(target, compact);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       int x, int y, int width, const char *value)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, width);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(o);
    lv_label_set_text(o, value ? value : "");
    return o;
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int width, int height,
                      uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, width, height);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_transform_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *pixel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = rect(parent, x, y, w, h, color, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void overlay_qr(lv_obj_t *field, lv_obj_t *qr, lv_obj_t *detail,
                       const char *payload, const char *failure)
{
    if (payload && *payload &&
        lv_qrcode_update(qr, payload, strlen(payload)) == LV_RES_OK) {
        lv_obj_clear_flag(field, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(field, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail, failure);
}

/* Opaque screen child. Neither QR payload is used as visible copy or logged. */
static lv_obj_t *create_qr_overlay(lv_obj_t *screen, lv_obj_t **title,
                                   lv_obj_t **detail, lv_obj_t **field,
                                   lv_obj_t **qr, lv_obj_t **name)
{
    lv_obj_t *overlay = rect(screen, 0, 0, 480, 480, BG, 0);
    *title = label(overlay, &usage_ui_font, TEXT, 24, 35, W, "");
    lv_obj_set_style_text_align(*title, LV_TEXT_ALIGN_CENTER, 0);
    *field = rect(overlay, 114, 99, 252, 252, 0xFFFFFF, 0);
    *qr = lv_qrcode_create(*field, 192, lv_color_hex(BG), lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(*qr, 30, 30);
    lv_obj_add_flag(*field, LV_OBJ_FLAG_HIDDEN);
    *detail = label(overlay, &usage_status_font, MUTED, 24, 372, W, "");
    lv_obj_set_style_text_align(*detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(*detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(*detail, 40);
    *name = label(overlay, &usage_meta_font, META, 24, 70, W, s_device_name);
    lv_obj_set_style_text_align(*name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    return overlay;
}

/* Squat, square-pixel Clawd. */
static void mascot(lv_obj_t *screen)
{
    s_mascot = lv_obj_create(screen);
    lv_obj_remove_style_all(s_mascot);
    lv_obj_set_pos(s_mascot, s_mascot_base_x, MASCOT_Y);
    lv_obj_set_size(s_mascot, 68, 56);
    lv_obj_set_style_bg_opa(s_mascot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_mascot, 0, 0);
    lv_obj_clear_flag(s_mascot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    pixel(s_mascot, 8, 8, 36, 24, ORANGE);
    pixel(s_mascot, 14, 4, 7, 4, ORANGE);
    pixel(s_mascot, 31, 4, 7, 4, ORANGE);
    s_mascot_left_arm = pixel(s_mascot, 4, 14, 4, 16, ORANGE);
    s_mascot_right_arm = pixel(s_mascot, 44, 14, 4, 16, ORANGE);
    s_mascot_left_foot = pixel(s_mascot, 14, 32, 6, 5, ORANGE);
    pixel(s_mascot, 23, 32, 6, 5, ORANGE);
    s_mascot_right_foot = pixel(s_mascot, 32, 32, 6, 5, ORANGE);
    pixel(s_mascot, 18, 16, 4, 5, BG);
    pixel(s_mascot, 30, 16, 4, 5, BG);
}

typedef struct {
    int8_t x, y;
    int8_t lifted_foot; /* -1 left, +1 right, 0 neither */
    int8_t left_arm_y, right_arm_y;
} mascot_frame_t;

/* Rest 1.2 s, walk and bob 2.4 s, rest 0.4 s. All coordinates are pixels. */
static const mascot_frame_t s_mascot_frames[] = {
    {0, -1, -1, -1,  1}, {1, -2,  1,  1, -1},
    {2, -3, -1, -2,  1}, {3, -4,  1,  1, -2},
    {4, -3, -1, -2,  1}, {6, -2,  1,  1, -2},
    {8, -1, -1, -1,  1}, {10, 0,  1,  1, -1},
    {12, 1, -1, -2,  1}, {12, 2,  1,  1, -2},
    {12, 3, -1, -2,  1}, {12, 4,  1,  1, -2},
    {11, 3, -1, -1,  1}, {10, 2,  1,  1, -1},
    {9, 1, -1, -2,  1}, {8, 0,  1,  1, -2},
    {7, -1, -1, -1,  1}, {6, -2,  1,  1, -1},
    {5, -3, -1, -2,  1}, {4, -2,  1,  1, -2},
    {3, -1, -1, -1,  1}, {2, 0,  1,  1, -1},
    {1, 0, -1, -1,  1}, {0, 0,  0,  0,  0},
};

static void mascot_reset(void)
{
    s_mascot_cycle_started_us = 0;
    if (s_mascot_frame == -1) return;
    lv_obj_set_pos(s_mascot, s_mascot_base_x, MASCOT_Y);
    lv_obj_set_y(s_mascot_left_foot, 32);
    lv_obj_set_y(s_mascot_right_foot, 32);
    lv_obj_set_y(s_mascot_left_arm, 14);
    lv_obj_set_y(s_mascot_right_arm, 14);
    s_mascot_frame = -1;
}

/* LVGL timer callbacks run under the port lock. No objects are created here. */
static void mascot_tick(lv_timer_t *timer)
{
    (void)timer;
    int64_t now = esp_timer_get_time();
    int64_t age = (now - s_measured_us) / 1000000;
    if (!box_config_current()->mascot_motion || s_rendered_view == VIEW_CLOCK ||
        !s_has_usage || s_setup_visible ||
        settings_overlay_open() || s_wifi_setup_active || s_updating || s_error[0] ||
        s_unknown_usage ||
        s_boot_active || s_face_active || s_refresh_pending || s_cycle_inflight ||
        age < 0 || age > STALE_SEC) {
        mascot_reset();
        return;
    }
    if (!s_mascot_cycle_started_us) s_mascot_cycle_started_us = now;
    int64_t cycle_ms = ((now - s_mascot_cycle_started_us) / 1000) % MASCOT_CYCLE_MS;
    int frame = cycle_ms < MASCOT_PLAY_START_MS ? -1 :
                (int)((cycle_ms - MASCOT_PLAY_START_MS) / MASCOT_TICK_MS);
    if (frame >= (int)(sizeof(s_mascot_frames) / sizeof(s_mascot_frames[0])))
        frame = -1;
    if (frame == s_mascot_frame) return;
    if (frame < 0) {
        mascot_reset();
        /* Keep this healthy cycle's phase through the short end rest. */
        s_mascot_cycle_started_us = now - cycle_ms * 1000;
        return;
    }
    const mascot_frame_t *pose = &s_mascot_frames[frame];
    lv_obj_set_pos(s_mascot, s_mascot_base_x + pose->x, MASCOT_Y + pose->y);
    lv_obj_set_y(s_mascot_left_foot, pose->lifted_foot < 0 ? 29 : 32);
    lv_obj_set_y(s_mascot_right_foot, pose->lifted_foot > 0 ? 29 : 32);
    lv_obj_set_y(s_mascot_left_arm, 14 + pose->left_arm_y);
    lv_obj_set_y(s_mascot_right_arm, 14 + pose->right_arm_y);
    s_mascot_frame = frame;
}

static bool selected_provider_message(char *out, size_t size)
{
    if (s_detail_app != APP_ID_CLAUDE && s_detail_app != APP_ID_CODEX)
        return false;
    int id = s_detail_app - 1;
    const provider_usage_t *data = &s_provider_data[id];
    if (data->status[0] && strcmp(data->status, "Ready") != 0) {
        snprintf(out, size, "%s: %s", usage_provider_name(id), data->status);
        return true;
    }
    if (data->updating) {
        snprintf(out, size, "%s: updating...", usage_provider_name(id));
        return true;
    }
    return false;
}

static bool selected_provider_stale(void)
{
    if (s_detail_app != APP_ID_CLAUDE && s_detail_app != APP_ID_CODEX)
        return false;
    int64_t now_us = esp_timer_get_time();
    const provider_usage_t *data = &s_provider_data[s_detail_app - 1];
    return data->has_sample &&
           (data->sampled_us <= 0 || data->sampled_us > now_us ||
            now_us - data->sampled_us > STALE_SEC * 1000000LL);
}

/* The shell is the only navigation authority. This function paints its route. */
static void render_shell_view(void)
{
    ui_shell_view_t state = ui_shell_view(&s_shell);
    rendered_view_t view = state.route == UI_ROUTE_SAVER ? VIEW_CLOCK :
                           state.route == UI_ROUTE_HOME ? VIEW_HOME :
                           state.route == UI_ROUTE_APP ? VIEW_APP : VIEW_STATUS;
    bool detail_changed = view == VIEW_APP && s_detail_app != state.app_id;
    if (detail_changed) {
        const app_descriptor_t *app = app_registry_find(&s_apps, state.app_id);
        if (app && app->mount_app) app->mount_app(app->context, s_content);
    }
    if (view == s_rendered_view && !detail_changed) return;
    s_rendered_view = view;
    bool clock = view == VIEW_CLOCK;
    lv_obj_t *chrome[] = {s_mascot, s_heading, s_mascot_hitbox,
                          s_home_hitbox, s_home_label,
                          s_header_refresh_hitbox, s_header_refresh_label,
                          s_header_settings_hitbox, s_header_settings_label};
    for (size_t i = 0; i < sizeof(chrome) / sizeof(chrome[0]); ++i) {
        if (!chrome[i]) continue;
        if (clock || view == VIEW_STATUS) lv_obj_add_flag(chrome[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(chrome[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (clock) mascot_reset();
    lv_obj_add_flag(s_provider_label, LV_OBJ_FLAG_HIDDEN);
    if (view == VIEW_APP && state.app_id > APP_ID_NONE &&
        state.app_id <= USAGE_PROVIDER_COUNT &&
        s_provider_data[state.app_id - 1].configured &&
        s_provider_data[state.app_id - 1].reset_credits_known)
        lv_obj_clear_flag(s_credits, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_credits, LV_OBJ_FLAG_HIDDEN);
    if (view == VIEW_HOME) {
        lv_obj_add_flag(s_home_hitbox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_home_label, LV_OBJ_FLAG_HIDDEN);
    }
    int mascot_x = view == VIEW_HOME ? 24 : MASCOT_X;
    if (s_mascot_base_x != mascot_x) {
        s_mascot_base_x = mascot_x;
        mascot_reset();
        lv_obj_set_x(s_mascot, mascot_x);
        lv_obj_set_x(s_mascot_hitbox, view == VIEW_HOME ? 16 : 60);
        lv_obj_set_width(s_mascot_hitbox, view == VIEW_HOME ? 80 : 56);
    }
    if (view == VIEW_HOME) {
        lv_label_set_text(s_heading, "Home");
        lv_obj_set_pos(s_heading, 96, 15);
        lv_obj_set_width(s_heading, 168);
        lv_obj_set_style_text_align(s_heading, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_clear_flag(s_home_layer, LV_OBJ_FLAG_HIDDEN);
        /* Widgets were updated while this parent was hidden by boot or a
         * screensaver. LVGL 8 does not always enqueue their child pixels. */
        lv_obj_invalidate(s_home_layer);
    } else lv_obj_add_flag(s_home_layer, LV_OBJ_FLAG_HIDDEN);
    if (view == VIEW_APP) {
        const app_descriptor_t *app = app_registry_find(&s_apps, state.app_id);
        lv_label_set_text(s_heading, app && app->name ? app->name : "App");
        lv_obj_set_pos(s_heading, 128, 15);
        lv_obj_set_width(s_heading, 136);
        lv_obj_set_style_text_align(s_heading, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (view == VIEW_APP || view == VIEW_STATUS) {
        lv_obj_clear_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_content);
    } else lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < screensaver_registry_count(&s_savers); ++i) {
        const screensaver_descriptor_t *saver = screensaver_registry_at(&s_savers, i);
        bool visible = clock && saver->id == state.saver_id;
        if (visible && saver->update) saver->update(saver->context);
        if (saver->set_visible) saver->set_visible(saver->context, visible);
    }
    if (clock) lv_obj_clear_flag(s_word_hitbox, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_word_hitbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_refresh_hitbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings_hitbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings_label, LV_OBJ_FLAG_HIDDEN);
    if (view == VIEW_APP) {
        if (lv_label_get_text(s_footer)[0])
            lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_updated_age, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_updated_age, LV_OBJ_FLAG_HIDDEN);
    }
    /* The port keeps a retained framebuffer. A route switch changes several
     * sibling layers; repaint the full frame once so unchanged header pixels
     * survive partial-buffer handoffs. This never runs on the timer's steady
     * state path because unchanged routes return above. */
    lv_obj_invalidate(lv_scr_act());
}

static void word_clicked(lv_event_t *event)
{
    (void)event;
    if (s_rendered_view != VIEW_CLOCK || s_wifi_setup_active || settings_overlay_open()) return;
    s_escaped_pending_mask = 0;
    ui_shell_navigate_home(&s_shell, esp_timer_get_time());
    render_shell_view();
    refresh(NULL);
}

static void update_word_clock(int hour, int minute, bool ready)
{
    if (s_word_render_hour == hour && s_word_render_minute == minute &&
        s_word_render_ready == ready) return;
    s_word_render_hour = hour;
    s_word_render_minute = minute;
    s_word_render_ready = ready;
    word_clock_state_t state;
    word_clock_compute_language(hour, minute, ready,
                                box_config_current()->clock_language, &state);
    for (int i = 0; i < WORD_CLOCK_CELLS; i++)
        lv_obj_set_style_text_color(s_word_letters[i],
                                    lv_color_hex(state.active[i] ? TEXT : WORD_INACTIVE), 0);
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_color(s_word_dots[i],
                                  lv_color_hex(i < state.minute_dots ? TEXT : WORD_INACTIVE), 0);
    if (state.valid) lv_obj_add_flag(s_word_wait, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_word_wait, LV_OBJ_FLAG_HIDDEN);
}

static int word_row_y(int row)
{
    return WORD_Y + (row * WORD_ROW_SPAN + (WORD_CLOCK_ROWS - 1) / 2) /
           (WORD_CLOCK_ROWS - 1);
}

static void face_eye(lv_obj_t *eye, int height)
{
    lv_obj_set_y(eye, 250 - height / 2);
    lv_obj_set_height(eye, height);
}

static int wink_height(int64_t elapsed, int start, int end)
{
    if (elapsed < start || elapsed >= end) return FACE_EYE_H;
    int t = (int)(elapsed - start);
    int duration = end - start;
    if (t < 90) return FACE_EYE_H - (FACE_EYE_H - 8) * t / 90;
    if (t < duration - 90) return 8;
    return 8 + (FACE_EYE_H - 8) * (t - (duration - 90)) / 90;
}

static void invalidate_visible_tree(lv_obj_t *object)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_invalidate(object);
    uint32_t count = lv_obj_get_child_cnt(object);
    for (uint32_t i = 0; i < count; ++i)
        invalidate_visible_tree(lv_obj_get_child(object, i));
}

static void repaint_dashboard_chrome(void)
{
    lv_obj_move_foreground(s_content);
    lv_obj_move_foreground(s_mascot);
    lv_obj_move_foreground(s_heading);
    lv_obj_move_foreground(s_provider_label);
    lv_obj_move_foreground(s_credits);
    lv_obj_move_foreground(s_footer);
    lv_obj_move_foreground(s_updated_age);
    lv_obj_move_foreground(s_settings_label);
    lv_obj_move_foreground(s_mascot_hitbox);
    lv_obj_move_foreground(s_refresh_hitbox);
    lv_obj_move_foreground(s_settings_hitbox);
    invalidate_visible_tree(s_content);
    lv_obj_invalidate(s_mascot);
    lv_obj_invalidate(s_heading);
    lv_obj_invalidate(s_provider_label);
    lv_obj_invalidate(s_credits);
    lv_obj_invalidate(s_footer);
    lv_obj_invalidate(s_updated_age);
    lv_obj_invalidate(s_settings_label);
}

static void stop_face(void)
{
    bool was_active = s_face_active;
    s_face_active = false;
    lv_timer_pause(s_face_timer);
    lv_obj_add_flag(s_face, LV_OBJ_FLAG_HIDDEN);
    /* The dashboard may have changed under the opaque face. */
    if (was_active && !settings_overlay_open() && !s_wifi_setup_active)
        repaint_dashboard_chrome();
    face_eye(s_face_left_eye, FACE_EYE_H);
    face_eye(s_face_right_eye, FACE_EYE_H);
    ui_shell_set_face(&s_shell, false, esp_timer_get_time());
    render_shell_view();
}

static void face_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_face_active) return;
    int64_t elapsed = (esp_timer_get_time() - s_face_started_us) / 1000;
    if (!s_boot_active && elapsed >= FACE_MS) {
        stop_face();
        return;
    }
    if (elapsed < 0) elapsed = 0;
    int opacity = LV_OPA_COVER;
    if (s_boot_active) {
        if (s_boot_ready && s_boot_fade_started_us &&
            esp_timer_get_time() >= s_boot_fade_started_us) {
            int64_t fade_elapsed = (esp_timer_get_time() - s_boot_fade_started_us) / 1000;
            if (fade_elapsed >= FACE_FADE_OUT_MS) {
                s_boot_active = false;
                ui_shell_set_boot(&s_shell, false, esp_timer_get_time());
                stop_face();
                return;
            }
            opacity = (int)((FACE_FADE_OUT_MS - fade_elapsed) * LV_OPA_COVER /
                            FACE_FADE_OUT_MS);
        }
    } else if (elapsed < FACE_FADE_IN_MS) {
        opacity = (int)(elapsed * LV_OPA_COVER / FACE_FADE_IN_MS);
    } else if (elapsed >= FACE_MS - FACE_FADE_OUT_MS) {
        opacity = (int)((FACE_MS - elapsed) * LV_OPA_COVER / FACE_FADE_OUT_MS);
    }
    lv_obj_set_style_bg_opa(s_face, opacity, 0);
    lv_obj_set_style_bg_opa(s_face_left_eye, opacity, 0);
    lv_obj_set_style_bg_opa(s_face_right_eye, opacity, 0);
    if (s_boot_active) lv_obj_set_style_text_opa(s_boot_label, opacity, 0);
    int both_height = wink_height(elapsed, 1550, 1840);
    int left_height = both_height < FACE_EYE_H
                          ? both_height : wink_height(elapsed, 2850, 3180);
    face_eye(s_face_left_eye, left_height);
    face_eye(s_face_right_eye, both_height);
}

static void start_face(void)
{
    mascot_reset();
    s_face_active = true;
    s_face_started_us = esp_timer_get_time();
    lv_obj_set_style_bg_opa(s_face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_face_left_eye, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_face_right_eye, LV_OPA_TRANSP, 0);
    face_eye(s_face_left_eye, FACE_EYE_H);
    face_eye(s_face_right_eye, FACE_EYE_H);
    lv_obj_add_flag(s_boot_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_face, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_face);
    lv_timer_reset(s_face_timer);
    lv_timer_resume(s_face_timer);
    ui_shell_set_face(&s_shell, true, esp_timer_get_time());
}

static void mascot_clicked(lv_event_t *event)
{
    (void)event;
    if ((s_rendered_view != VIEW_APP && s_rendered_view != VIEW_HOME) ||
        settings_overlay_open() || s_wifi_setup_active || s_boot_active ||
        s_face_active) return;
    ui_shell_mark_user_refresh(&s_shell, esp_timer_get_time());
    start_face();
}

static void boot_status(const char *message)
{
    lv_label_set_text(s_boot_label, message);
}

static bool boot_progress(const char *title, const char *detail)
{
    if (!s_boot_active || !title) return false;
    if (strcmp(title, "Starting") == 0 ||
        (strcmp(title, "Wi-Fi") == 0 && detail &&
         (strcmp(detail, "Reconnecting") == 0 ||
          strcmp(detail, "Waiting for connection") == 0))) {
        boot_status("Connecting to Wi-Fi...");
        return true;
    }
    if (strcmp(title, "Time") == 0 && detail &&
        strcmp(detail, "Waiting for clock") == 0) {
        boot_status("Waiting for time...");
        return true;
    }
    if (strcmp(title, "OAuth") == 0 && detail &&
        strcmp(detail, "Refreshing token") == 0) {
        boot_status("Loading usage...");
        return true;
    }
    return false;
}

static void clear_content(void)
{
    s_setup_name = NULL;
    lv_obj_clean(s_content);
    memset(&s_five, 0, sizeof(s_five));
    memset(&s_week, 0, sizeof(s_week));
    s_detail_app = APP_ID_NONE;
}

static void footer(const char *message)
{
    if (!message[0]) {
        if (lv_label_get_text(s_footer)[0] ||
            !lv_obj_has_flag(s_footer, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_invalidate(s_footer);
            lv_label_set_text(s_footer, "");
            lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_set_y(s_footer, strlen(message) > 48 ? 405 : 418);
    lv_obj_set_height(s_footer, 34);
    if (strcmp(lv_label_get_text(s_footer), message) != 0)
        lv_label_set_text(s_footer, message);
    if (s_rendered_view == VIEW_APP)
        lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
}

static void footer_clicked(lv_event_t *event)
{
    (void)event;
    ui_shell_view_t route = ui_shell_view(&s_shell);
    if ((route.route != UI_ROUTE_HOME && route.route != UI_ROUTE_APP) ||
        settings_overlay_open() || s_wifi_setup_active || s_boot_active ||
        s_face_active || s_refresh_pending || s_cycle_inflight) return;
    app_id_t id = route.route == UI_ROUTE_APP ? route.app_id : APP_ID_ALL;
    s_refresh_pending_mask = 0;
    uint32_t accepted_mask = s_app_refresh_callback ?
        s_app_refresh_callback(s_app_refresh_context, id) :
        id == APP_ID_ALL && s_refresh_callback && s_refresh_callback(s_refresh_context)
            ? (1u << APP_ID_CLAUDE) : 0;
    accepted_mask &= (1u << APP_ID_CLAUDE) | (1u << APP_ID_CODEX);
    if (id != APP_ID_ALL) accepted_mask &= 1u << id;
    s_refresh_pending_mask = accepted_mask;
    s_refresh_pending = accepted_mask != 0;
    if (accepted_mask) {
        mascot_reset();
        ui_shell_mark_user_refresh(&s_shell, esp_timer_get_time());
        if (box_config_current()->refresh_animation) start_face();
    }
    refresh(NULL);
}

static void home_clicked(lv_event_t *event)
{
    (void)event;
    if (settings_overlay_open() || s_wifi_setup_active || s_boot_active) return;
    s_escaped_pending_mask = 0;
    if (s_face_active) stop_face();
    ui_shell_navigate_home(&s_shell, esp_timer_get_time());
    render_shell_view();
}

static void home_widget_clicked(lv_event_t *event)
{
    display_app_context_t *context = lv_event_get_user_data(event);
    if (!context || settings_overlay_open() || s_wifi_setup_active || s_boot_active) return;
    s_escaped_pending_mask = 0;
    if (!usage_provider_enabled(box_config_current()->providers, context->provider) ||
        !s_provider_data[context->provider].configured) {
        open_web_setup(context->provider);
        return;
    }
    ui_shell_navigate_app(&s_shell, (app_id_t)context->provider + 1,
                          esp_timer_get_time());
    render_shell_view();
    refresh(NULL);
}

static void close_settings(bool notify_backend)
{
    if (s_web_setup_active) {
        s_web_setup_active = false;
        lv_obj_add_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_settings_detail, 24, 372);
        lv_obj_set_height(s_settings_detail, 40);
        ui_shell_set_settings(&s_shell, false, esp_timer_get_time());
        return;
    }
    if (!s_settings_active) return;
    uint32_t request_id = s_settings_request_id;
    /* Once OPEN was accepted, the worker may already be committing a new
     * credential. A rejected CLOSE keeps Back available until it finishes. */
    if (notify_backend && request_id && s_settings_accepted &&
        s_settings_callback &&
        !s_settings_callback(s_settings_context, request_id, false)) return;
    /* An unavailable OPEN has no session to protect: Back always dismisses it. */
    if (notify_backend && request_id && !s_settings_accepted && s_settings_callback)
        (void)s_settings_callback(s_settings_context, request_id, false);
    s_settings_active = false;
    s_settings_accepted = false;
    s_settings_request_id = 0;
    lv_obj_add_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    ui_shell_set_settings(&s_shell, false, esp_timer_get_time());
}

static void settings_back_clicked(lv_event_t *event)
{
    (void)event;
    close_settings(true);
    refresh(NULL);
}

static void settings_clicked(lv_event_t *event)
{
    (void)event;
    if ((s_rendered_view != VIEW_APP && s_rendered_view != VIEW_HOME) ||
        s_settings_active || s_web_setup_active || s_wifi_setup_active ||
        s_boot_active || s_face_active) return;
    s_settings_request_id = s_settings_sequence == UINT32_MAX
                                ? 0 : ++s_settings_sequence;
    s_settings_active = true;
    s_settings_accepted = false;
    ui_shell_set_settings(&s_shell, true, esp_timer_get_time());
    lv_label_set_text(s_settings_title, "Settings");
    lv_label_set_text(s_settings_detail, "Opening...");
    lv_obj_add_flag(s_settings_qr_field, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_overlay);
    if (!s_settings_request_id || !s_settings_callback ||
        !s_settings_callback(s_settings_context, s_settings_request_id, true))
        lv_label_set_text(s_settings_detail, "Settings unavailable");
    else
        s_settings_accepted = true;
}

/* Device configuration, including Codex sign-in, lives in the installer.
 * This public URL is distinct from the capability-protected account page. */
static void open_web_setup(usage_provider_id_t provider)
{
    if (s_settings_active || s_web_setup_active || s_wifi_setup_active ||
        s_boot_active || s_face_active) return;
    s_web_setup_active = true;
    ui_shell_set_settings(&s_shell, true, esp_timer_get_time());
    char title[36], detail[112];
    snprintf(title, sizeof(title), "Set up %s", usage_provider_name(provider));
    snprintf(detail, sizeof(detail),
             "Connect USB in Chrome or Edge. Enable/sign in to %s in web app.",
             usage_provider_name(provider));
    lv_label_set_text(s_settings_title, title);
    lv_label_set_text(s_settings_detail, detail);
    lv_obj_set_pos(s_settings_detail, 24, 362);
    lv_obj_set_height(s_settings_detail, 48);
    overlay_qr(s_settings_qr_field, s_settings_qr, s_settings_detail,
               "https://hello-box-installer.vercel.app", "Installer QR unavailable");
    lv_obj_clear_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_overlay);
}

/* LVGL timer callbacks run on the LVGL task, under the port's own lock. */
static void refresh(lv_timer_t *timer)
{
    (void)timer;
    bool time_valid = false;
    time_t now = time(NULL);
    if (s_clock_ready) {
        struct tm local;
        time_valid = localtime_r(&now, &local) && local.tm_year >= 125;
    }
    s_time_valid = time_valid;
    ui_shell_view_t route = ui_shell_view(&s_shell);
    if (route.route == UI_ROUTE_SAVER) {
        const screensaver_descriptor_t *saver = screensaver_registry_find(&s_savers,
                                                      route.saver_id);
        if (saver && saver->update) saver->update(saver->context);
    }
    int64_t now_us = esp_timer_get_time();
    ui_shell_tick(&s_shell, now_us);
    render_shell_view();
    for (int id = 0; id < USAGE_PROVIDER_COUNT; ++id)
        usage_app_widget_update(&s_app_contexts[id].widget, &s_provider_data[id],
            usage_provider_enabled(box_config_current()->providers, id),
            time_valid, (int64_t)now, now_us);
    if (s_rendered_view != VIEW_APP || !s_has_usage) return;
    int64_t age = (esp_timer_get_time() - s_measured_us) / 1000000;

    bool measurement_age_valid = age >= 0 && age <= STALE_SEC;
    if (age < 0) age = 0;
    bool stale = selected_provider_stale();
    bool forecast_fresh = measurement_age_valid && time_valid && !s_error[0] &&
                          !s_setup_visible;
    if (s_five.value) {
        update_forecast_icon(&s_five, now, forecast_fresh, s_five.window_seconds);
        lv_obj_set_style_bg_opa(s_five.fill, stale ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
    if (s_week.value) {
        update_forecast_icon(&s_week, now, forecast_fresh, s_week.window_seconds);
        lv_obj_set_style_bg_opa(s_week.fill, stale ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
    char age_text[32];
    if (s_has_sample) display_format_age(age_text, sizeof(age_text), age);
    else snprintf(age_text, sizeof(age_text), "Never updated");
    if (strcmp(lv_label_get_text(s_updated_age), age_text) != 0)
        lv_label_set_text(s_updated_age, age_text);
    lv_obj_clear_flag(s_updated_age, LV_OBJ_FLAG_HIDDEN);

    char message[96];
    char provider_message[96];
    bool provider_attention = selected_provider_message(provider_message,
                                                        sizeof(provider_message));
    if (s_error[0]) {
        snprintf(message, sizeof(message), "%s: %s", s_status_prefix, s_error);
    } else if (stale) {
        snprintf(message, sizeof(message), "%s",
                 s_updating || s_refresh_pending || s_cycle_inflight
                     ? "Stale - updating..." : "Stale");
    } else if (s_updating || s_refresh_pending || s_cycle_inflight) {
        snprintf(message, sizeof(message), "Updating...");
    } else if (provider_attention) {
        snprintf(message, sizeof(message), "%s", provider_message);
    } else if (!time_valid) {
        snprintf(message, sizeof(message), "Waiting for time");
    } else if (s_unknown_usage) {
        int selection = box_config_current()->providers;
        if (s_detail_app == APP_ID_CODEX &&
            s_provider_data[USAGE_PROVIDER_CODEX].configured &&
            !usage_provider_weekly_window(&s_provider_data[USAGE_PROVIDER_CODEX]))
            message[0] = '\0'; /* The detail already says what is unavailable. */
        else if (selection == 1 && !s_provider_data[USAGE_PROVIDER_CODEX].configured)
            snprintf(message, sizeof(message), "Set up Codex in web app");
        else if (selection < 2 && s_provider_data[selection].status[0])
            snprintf(message, sizeof(message), "%s", s_provider_data[selection].status);
        else snprintf(message, sizeof(message), "Unknown");
    } else message[0] = '\0';
    footer(message);

    if (s_five.reset) {
        update_reset_label(s_five.reset, s_five.reset_iso, now, s_clock_ready);
    }
    if (s_week.reset) {
        update_reset_label(s_week.reset, s_week.reset_iso, now, s_clock_ready);
    }
}

static void status(const char *title, const char *detail)
{
    clear_content();
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_updated_age, LV_OBJ_FLAG_HIDDEN);
    label(s_content, &usage_ui_font, TEXT, 0, 24, W,
          title && *title ? title : "Starting");
    lv_obj_t *description = label(s_content, &usage_ui_font, MUTED, 0, 76, W,
                                  detail && *detail ? detail : "Starting...");
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(description, 116);
}

static card_t create_card(int y, const char *period, uint32_t color)
{
    card_t c = {0};
    lv_obj_t *box = rect(s_content, 0, y, W, CARD_H, BG, 0);
    c.root = box;
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    c.value = label(box, &usage_values_font, TEXT, CARD_PAD, 7, 240, "--%");
    c.period = label(box, &usage_status_font, MUTED, 284, 20, 132, period);
    lv_obj_set_style_text_align(c.period, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *icon = icon_container(box);
    c.forecast_root = icon;
    lv_obj_set_pos(icon, 264, 24);
    c.forecast_headroom = forecast_circle(icon, FORECAST_GREEN);
    c.forecast_tight = forecast_circle(icon, ORANGE);
    c.forecast_unknown = forecast_circle(icon, FORECAST_NEUTRAL);
    lv_obj_add_flag(c.forecast_headroom, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c.forecast_tight, LV_OBJ_FLAG_HIDDEN);
    c.fill = rect(box, CARD_PAD, 66, 0, 24, color, 4);
    lv_obj_clear_flag(c.fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *clock = icon_container(box);
    c.reset_icon = clock;
    lv_obj_set_pos(clock, CARD_PAD, 100);
    lv_obj_t *outline = rect(clock, 0, 0, 16, 16, BG, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(outline, 2, 0);
    lv_obj_set_style_border_color(outline, lv_color_hex(MUTED), 0);
    lv_obj_set_style_border_opa(outline, LV_OPA_COVER, 0);
    lv_obj_clear_flag(outline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *hour_hand = rect(clock, 7, 3, 2, 6, MUTED, 0);
    lv_obj_t *minute_hand = rect(clock, 7, 7, 5, 2, MUTED, 0);
    lv_obj_clear_flag(hour_hand, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(minute_hand, LV_OBJ_FLAG_CLICKABLE);
    c.reset = label(box, &usage_status_font, MUTED, CARD_PAD + 24, 98,
                    BAR_W - 24, "Unknown");
    return c;
}

static void update_card(card_t *card, float used, const char *reset)
{
    char value[16];
    float remaining = NAN;
    if (isfinite(used) && used >= 0.0f && used <= 100.0f) {
        remaining = 100.0f - used;
        snprintf(value, sizeof(value), "%.0f%%", remaining);
    } else {
        snprintf(value, sizeof(value), "--%%");
    }
    lv_label_set_text(card->value, value);
    lv_obj_set_width(card->fill, isfinite(remaining) ? (lv_coord_t)lroundf(remaining * BAR_W / 100.0f) : 0);
    lv_obj_set_style_bg_color(card->fill,
                              lv_color_hex(display_quota_color(remaining)), 0);
    snprintf(card->reset_iso, sizeof(card->reset_iso), "%s", reset ? reset : "");
}

void display_init(void)
{
    app_registry_reset(&s_apps);
    for (int i = 0; i < USAGE_PROVIDER_COUNT; ++i) {
        const app_descriptor_t descriptor = {
            .id = (app_id_t)i + 1,
            .name = usage_provider_name(i),
            .capabilities = APP_CAP_REFRESH | APP_CAP_INTERRUPT_SAVER,
            .context = &s_app_contexts[i],
            .enabled = app_enabled,
            .configured = app_configured,
            .request_refresh = app_request_refresh,
            .mount_widget = app_mount_widget,
            .mount_app = app_mount_detail,
            .update_views = app_update_views,
        };
        ESP_ERROR_CHECK(app_registry_register(&s_apps, &descriptor) ? ESP_OK : 1);
    }
    screensaver_registry_reset(&s_savers);
    const screensaver_descriptor_t word_saver = {
        .id = SAVER_ID_WORD_CLOCK, .name = "Word clock",
        .mount = saver_mount, .update = saver_update, .set_visible = saver_visible,
    };
    ESP_ERROR_CHECK(screensaver_registry_register(&s_savers, &word_saver)
                    ? ESP_OK : 1);
    ESP_ERROR_CHECK(ui_shell_init(&s_shell, &s_apps, &s_savers,
        box_config_current()->default_view,
        box_config_current()->dashboard_timeout_sec,
        SAVER_ID_WORD_CLOCK, esp_timer_get_time()) ? ESP_OK : 1);
    ui_shell_set_boot(&s_shell, true, esp_timer_get_time());
    ESP_ERROR_CHECK(bsp_board_init());
    lv_port_init();
    lv_port_set_brightness(box_config_current()->brightness);
    setenv("TZ", box_config_timezone(), 1);
    tzset();

    lv_port_sem_take();
    s_setup_name = NULL;
    lv_obj_t *screen = lv_scr_act();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    mascot(screen);
    s_heading = label(screen, &usage_ui_font, TEXT, 118, 15, 158, "Home");
    lv_obj_set_style_text_align(s_heading, LV_TEXT_ALIGN_CENTER, 0);
    s_provider_label = label(screen, &usage_status_font, META, 40, 82, 180, "");
    lv_obj_add_flag(s_provider_label, LV_OBJ_FLAG_HIDDEN);
    s_credits = label(screen, &usage_meta_font, META, 245, 84, 195, "");
    lv_obj_set_style_text_align(s_credits, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(s_credits, LV_OBJ_FLAG_HIDDEN);

    s_home_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_home_layer);
    lv_obj_set_pos(s_home_layer, 0, 56);
    lv_obj_set_size(s_home_layer, 480, 424);
    lv_obj_set_style_bg_opa(s_home_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_home_layer, 0, 0);
    lv_obj_clear_flag(s_home_layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    size_t app_count = app_registry_count(&s_apps);
    if (app_count > 4) {
        lv_obj_add_flag(s_home_layer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_home_layer, LV_DIR_VER);
    }
    for (size_t i = 0; i < app_count; ++i) {
        const app_descriptor_t *app = app_registry_at(&s_apps, i);
        if (!app || !app->mount_widget) continue;
        int col = (int)(i % 2), row = (int)(i / 2);
        int width = app_count <= 2 ? 464 : 228;
        int height = 200;
        lv_obj_t *slot = lv_obj_create(s_home_layer);
        lv_obj_remove_style_all(slot);
        lv_obj_set_pos(slot, app_count <= 2 ? 8 : 8 + col * 236,
                       8 + (app_count <= 2 ? (int)i : row) * 208);
        lv_obj_set_size(slot, width, height);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_update_layout(slot);
        app->mount_widget(app->context, slot);
    }
    lv_obj_add_flag(s_home_layer, LV_OBJ_FLAG_HIDDEN);

    s_content = lv_obj_create(screen);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_pos(s_content, 24, 90);
    lv_obj_set_size(s_content, W, 344);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_outline_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < screensaver_registry_count(&s_savers); ++i) {
        const screensaver_descriptor_t *saver = screensaver_registry_at(&s_savers, i);
        if (saver && saver->mount) saver->mount(saver->context, screen);
    }
    s_footer = label(screen, &usage_status_font, ORANGE, 24, 418, W, "");
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_footer, 34);
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    s_updated_age = label(screen, &usage_meta_font, META, 240, 442, 216, "");
    lv_obj_set_style_text_align(s_updated_age, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_height(s_updated_age, 14);
    lv_obj_add_flag(s_updated_age, LV_OBJ_FLAG_HIDDEN);
    s_refresh_hitbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_refresh_hitbox);
    lv_obj_set_pos(s_refresh_hitbox, 112, 405);
    lv_obj_set_size(s_refresh_hitbox, 344, 51);
    lv_obj_clear_flag(s_refresh_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_refresh_hitbox, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_refresh_hitbox, footer_clicked, LV_EVENT_CLICKED, NULL);
    s_settings_label = label(screen, &usage_meta_font, META, 24, 442, 84, "Settings");
    lv_obj_add_flag(s_settings_label, LV_OBJ_FLAG_HIDDEN);
    s_settings_hitbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_settings_hitbox);
    lv_obj_set_pos(s_settings_hitbox, 24, 416);
    lv_obj_set_size(s_settings_hitbox, 84, 48);
    lv_obj_set_style_bg_opa(s_settings_hitbox, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_settings_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_settings_hitbox, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_settings_hitbox, settings_clicked, LV_EVENT_CLICKED, NULL);
    s_word_hitbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_word_hitbox);
    lv_obj_set_size(s_word_hitbox, 480, 480);
    lv_obj_set_style_bg_opa(s_word_hitbox, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_word_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_word_hitbox, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_word_hitbox, word_clicked, LV_EVENT_CLICKED, NULL);

    /* Separate screen child: clearing card content never destroys the face. */
    s_face = rect(screen, 0, 0, 480, 480, ORANGE, 0);
    lv_obj_clear_flag(s_face, LV_OBJ_FLAG_CLICKABLE);
    s_face_left_eye = rect(s_face, 136, 186, FACE_EYE_W, FACE_EYE_H, FACE_INK, 0);
    s_face_right_eye = rect(s_face, 296, 186, FACE_EYE_W, FACE_EYE_H, FACE_INK, 0);
    s_boot_label = label(s_face, &usage_status_font, FACE_INK, 24, 366, W,
                         "Connecting to Wi-Fi...");
    lv_obj_set_style_text_align(s_boot_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_boot_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_boot_label, 52);
    s_boot_active = true;
    s_face_active = true;
    s_face_started_us = esp_timer_get_time();
    /* Fixed targets stay put while the mascot moves. */
    s_mascot_hitbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_mascot_hitbox);
    lv_obj_set_pos(s_mascot_hitbox, 60, 0);
    lv_obj_set_size(s_mascot_hitbox, 56, 56);
    lv_obj_set_style_bg_opa(s_mascot_hitbox, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_mascot_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mascot_hitbox, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_mascot_hitbox, mascot_clicked, LV_EVENT_CLICKED, NULL);
    s_home_label = label(screen, &lv_font_montserrat_24, TEXT,
                         8, 13, 48, LV_SYMBOL_HOME);
    lv_label_set_long_mode(s_home_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_home_label, LV_SYMBOL_HOME);
    lv_obj_set_style_text_align(s_home_label, LV_TEXT_ALIGN_CENTER, 0);
    s_home_hitbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_home_hitbox);
    lv_obj_set_pos(s_home_hitbox, 8, 0);
    lv_obj_set_size(s_home_hitbox, 48, 56);
    lv_obj_add_flag(s_home_hitbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_home_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_home_hitbox, home_clicked, LV_EVENT_CLICKED, NULL);
    s_header_refresh_label = label(screen, &lv_font_montserrat_24, TEXT,
                                    356, 13, 48, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_align(s_header_refresh_label, LV_TEXT_ALIGN_CENTER, 0);
    s_header_refresh_hitbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_header_refresh_hitbox);
    lv_obj_set_pos(s_header_refresh_hitbox, 356, 0);
    lv_obj_set_size(s_header_refresh_hitbox, 48, 56);
    lv_obj_add_flag(s_header_refresh_hitbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_header_refresh_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_header_refresh_hitbox, footer_clicked, LV_EVENT_CLICKED, NULL);
    s_header_settings_label = label(screen, &lv_font_montserrat_24, TEXT,
                                     408, 13, 48, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_align(s_header_settings_label, LV_TEXT_ALIGN_CENTER, 0);
    s_header_settings_hitbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_header_settings_hitbox);
    lv_obj_set_pos(s_header_settings_hitbox, 408, 0);
    lv_obj_set_size(s_header_settings_hitbox, 48, 56);
    lv_obj_add_flag(s_header_settings_hitbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_header_settings_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_header_settings_hitbox, settings_clicked, LV_EVENT_CLICKED, NULL);
    status("Starting", "Connecting to Claude...");
    lv_timer_create(refresh, 1000, NULL);
    lv_timer_create(mascot_tick, MASCOT_TICK_MS, NULL);
    s_face_timer = lv_timer_create(face_tick, FACE_TICK_MS, NULL);

    s_settings_overlay = create_qr_overlay(screen, &s_settings_title,
                                            &s_settings_detail, &s_settings_qr_field,
                                            &s_settings_qr, &s_settings_name);
    lv_obj_t *back = rect(s_settings_overlay, 24, 416, 96, 48, CARD, 5);
    lv_obj_add_event_cb(back, settings_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = label(back, &usage_status_font, TEXT, 0, 15, 96, "Back");
    lv_obj_set_style_text_align(back_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(back_label, LV_OBJ_FLAG_CLICKABLE);

    s_wifi_overlay = create_qr_overlay(screen, &s_wifi_title,
                                        &s_wifi_detail, &s_wifi_qr_field, &s_wifi_qr,
                                        &s_wifi_name);
    /* The shared overlay is left untouched for Settings. Only the Wi-Fi copy
     * and QR field move; the 252 px white field keeps its 30 px quiet zone. */
    lv_obj_set_pos(s_wifi_title, 24, 12);
    lv_label_set_text(s_wifi_title, "Wi-Fi setup");
    lv_obj_set_pos(s_wifi_qr_field, 114, 112);
    lv_obj_set_pos(s_wifi_name, 24, 373);
    lv_obj_set_style_text_font(s_wifi_name, &usage_ui_font, 0);
    lv_obj_set_style_text_color(s_wifi_name, lv_color_hex(TEXT), 0);
    lv_obj_set_pos(s_wifi_detail, 24, 427);
    lv_obj_set_height(s_wifi_detail, 42);
    lv_label_set_long_mode(s_wifi_detail, LV_LABEL_LONG_DOT);
    s_wifi_step1_mark = pixel(s_wifi_overlay, 24, 48, 4, 19, ORANGE);
    s_wifi_step2_mark = pixel(s_wifi_overlay, 24, 77, 4, 19, META);
    s_wifi_step1 = label(s_wifi_overlay, &usage_status_font, TEXT, 40, 48, 328,
                         "1. Connect to this Wi-Fi");
    s_wifi_step2 = label(s_wifi_overlay, &usage_status_font, META, 40, 77, 328,
                         "2. Open the setup page");
    s_wifi_done = label(s_wifi_overlay, &usage_status_font, WEEK, 371, 48, 85,
                        "Done");
    lv_obj_add_flag(s_wifi_done, LV_OBJ_FLAG_HIDDEN);
    s_wifi_scan_hint = label(s_wifi_overlay, &usage_status_font, MUTED, 24, 402,
                             W, "Scan QR to join Wi-Fi");
    lv_obj_set_style_text_align(s_wifi_scan_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_port_sem_give();
}

void display_set_device_name(const char *name)
{
    if (!name || strlen(name) != 14 || strncmp(name, "hello-box-", 10) != 0) return;
    for (int i = 10; i < 14; ++i)
        if (name[i] < '0' || name[i] > '9') return;
    lv_port_sem_take();
    memcpy(s_device_name, name, sizeof(s_device_name));
    if (s_settings_name) lv_label_set_text(s_settings_name, s_device_name);
    if (s_wifi_name) lv_label_set_text(s_wifi_name, s_device_name);
    if (s_setup_name) lv_label_set_text(s_setup_name, s_device_name);
    lv_port_sem_give();
}

void display_status(const char *title, const char *detail)
{
    lv_port_sem_take();
    mascot_reset();
    if (boot_progress(title, detail)) {
        lv_port_sem_give();
        return;
    }
    s_boot_active = false;
    ui_shell_set_boot(&s_shell, false, esp_timer_get_time());
    bool refreshing = detail && strstr(detail, "Refreshing token");
    bool waiting_for_time = (title && strcmp(title, "Time") == 0) ||
                            (detail && strstr(detail, "Waiting for clock"));
    bool usage_retry = title && strcmp(title, "Usage") == 0 && detail &&
                       strstr(detail, "Retrying after cooldown");
    if (!refreshing) {
        s_updating = false;
        s_refresh_pending = false;
        s_refresh_pending_mask = 0;
        s_escaped_pending_mask = 0;
        s_cycle_inflight = false;
        stop_face();
    }
    if (s_has_usage) {
        snprintf(s_error, sizeof(s_error), "%s", detail && *detail ? detail :
                 (title && *title ? title : "Update failed"));
        s_status_prefix = title && strstr(title, "Wi-Fi")
                              ? "Offline"
                              : refreshing ? "Updating"
                              : waiting_for_time ? "Waiting"
                              : usage_retry ? "Retry" : "Error";
        refresh(NULL);
    } else {
        s_setup_visible = false;
        ui_shell_show_status(&s_shell, esp_timer_get_time());
        render_shell_view();
        status(title, detail);
    }
    lv_port_sem_give();
}

void display_setup(const char *url)
{
    lv_port_sem_take();
    mascot_reset();
    s_boot_active = false;
    s_has_usage = false;
    s_has_sample = false;
    s_updating = false;
    s_setup_visible = true;
    s_refresh_pending = false;
    s_refresh_pending_mask = 0;
    s_escaped_pending_mask = 0;
    s_cycle_inflight = false;
    ui_shell_set_boot(&s_shell, false, esp_timer_get_time());
    stop_face();
    ui_shell_show_status(&s_shell, esp_timer_get_time());
    render_shell_view();
    if (!url || !*url) {
        status("Connect Claude", "Setup link unavailable. Check Wi-Fi.");
        lv_port_sem_give();
        return;
    }
    clear_content();
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_updated_age, LV_OBJ_FLAG_HIDDEN);
    label(s_content, &usage_ui_font, TEXT, 0, 0, W, "Connect Claude");
    s_setup_name = label(s_content, &usage_meta_font, META, 0, 30, W,
                         s_device_name);
    lv_obj_set_style_text_align(s_setup_name, LV_TEXT_ALIGN_CENTER, 0);
    /* White 252 px field, 192 px code, 30 px quiet zone on each side. */
    lv_obj_t *field = rect(s_content, 90, 50, 252, 252, 0xFFFFFF, 0);
    lv_obj_t *qr = lv_qrcode_create(field, 192, lv_color_hex(BG), lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(qr, 30, 30);
    if (lv_qrcode_update(qr, url, strlen(url)) != LV_RES_OK) {
        status("Connect Claude", "Setup QR unavailable. Check the local link.");
        lv_port_sem_give();
        return;
    }
    lv_obj_t *hint = label(s_content, &usage_ui_font, MUTED,
                           0, 310, W, "Scan with your phone");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_port_sem_give();
}

void display_setup_closed(void)
{
    lv_port_sem_take();
    if (s_setup_visible) {
        s_setup_visible = false;
        s_has_usage = true;
        ui_shell_navigate_home(&s_shell, esp_timer_get_time());
        render_shell_view();
        refresh(NULL);
    }
    lv_port_sem_give();
}

void display_set_settings_callback(display_settings_callback_t callback, void *context)
{
    lv_port_sem_take();
    if (!callback) close_settings(false);
    s_settings_callback = callback;
    s_settings_context = context;
    lv_port_sem_give();
}

void display_settings_qr(uint32_t request_id, const char *url)
{
    lv_port_sem_take();
    if (s_settings_active && s_settings_accepted &&
        request_id == s_settings_request_id &&
        !s_wifi_setup_active) {
        lv_label_set_text(s_settings_detail, "Scan with your phone");
        overlay_qr(s_settings_qr_field, s_settings_qr, s_settings_detail,
                   url, "Settings unavailable");
    }
    lv_port_sem_give();
}

void display_settings_closed(uint32_t request_id)
{
    lv_port_sem_take();
    if (s_settings_active && request_id == s_settings_request_id)
        close_settings(false);
    lv_port_sem_give();
}

void display_wifi_setup(const char *join_qr, const char *page_url,
                        bool phone_joined, const char *message)
{
    lv_port_sem_take();
    if (!s_wifi_setup_active) {
        s_wifi_setup_active = true;
        close_settings(false);
        s_boot_active = false;
        ui_shell_set_boot(&s_shell, false, esp_timer_get_time());
        stop_face();
        ui_shell_set_wifi(&s_shell, true, esp_timer_get_time());
    }
    lv_obj_set_style_bg_color(s_wifi_step1_mark,
                              lv_color_hex(phone_joined ? WEEK : ORANGE), 0);
    lv_obj_set_style_bg_color(s_wifi_step2_mark,
                              lv_color_hex(phone_joined ? ORANGE : META), 0);
    lv_obj_set_style_text_color(s_wifi_step1,
                                lv_color_hex(phone_joined ? MUTED : TEXT), 0);
    lv_obj_set_style_text_color(s_wifi_step2,
                                lv_color_hex(phone_joined ? TEXT : META), 0);
    if (phone_joined) lv_obj_clear_flag(s_wifi_done, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_wifi_done, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_wifi_scan_hint,
                      phone_joined ? "Scan QR to open page" : "Scan QR to join Wi-Fi");
    lv_label_set_text(s_wifi_detail,
                      message && *message ? message :
                      phone_joined ? "Scan to open setup" : "Scan to join setup Wi-Fi");
    overlay_qr(s_wifi_qr_field, s_wifi_qr, s_wifi_detail,
               phone_joined ? page_url : join_qr, "Setup QR unavailable");
    lv_obj_clear_flag(s_wifi_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wifi_overlay);
    /* A QR update can invalidate only part of the overlay. Refresh the whole
     * Wi-Fi screen so its title and steps survive consecutive QR swaps. */
    lv_obj_invalidate(s_wifi_overlay);
    lv_port_sem_give();
}

void display_wifi_setup_done(void)
{
    lv_port_sem_take();
    if (s_wifi_setup_active) {
        s_wifi_setup_active = false;
        lv_obj_add_flag(s_wifi_overlay, LV_OBJ_FLAG_HIDDEN);
        ui_shell_set_wifi(&s_shell, false, esp_timer_get_time());
        refresh(NULL);
    }
    lv_port_sem_give();
}

void display_provider_update(usage_provider_id_t id, const provider_usage_t *usage)
{
    if (!usage || id < 0 || id >= USAGE_PROVIDER_COUNT) return;
    provider_usage_t snapshot = *usage;
    usage_provider_validate(&snapshot);
    lv_port_sem_take();
    bool fresh_sample = snapshot.configured && snapshot.has_sample &&
        snapshot.sampled_us > 0 &&
        (!s_provider_data[id].has_sample ||
         snapshot.sampled_us > s_provider_data[id].sampled_us) &&
        (id == USAGE_PROVIDER_CODEX ?
            usage_provider_weekly_window(&snapshot) != NULL :
            (snapshot.windows[0].valid || snapshot.windows[1].valid)) &&
        !snapshot.updating &&
        (!snapshot.status[0] || strcmp(snapshot.status, "Ready") == 0);
    s_provider_data[id] = snapshot;
    const app_descriptor_t *app = app_registry_find(&s_apps, (app_id_t)id + 1);
    if (app && app->update_views) app->update_views(app->context);
    if (fresh_sample)
        ui_shell_fresh_sample(&s_shell, (app_id_t)id + 1,
                              snapshot.sampled_us, esp_timer_get_time());
    if (usage_provider_enabled(box_config_current()->providers, id)) {
        s_has_usage = true;
        if (fresh_sample) {
            if (ui_shell_view(&s_shell).route == UI_ROUTE_STATUS && !s_boot_active)
                ui_shell_navigate_home(&s_shell, esp_timer_get_time());
            s_setup_visible = false;
            s_error[0] = '\0';
        }
    }
    if (s_boot_active && usage_provider_enabled(box_config_current()->providers, id) &&
        (snapshot.has_sample || !snapshot.configured || snapshot.status[0]) &&
        !s_boot_ready) {
        s_boot_ready = true;
        int64_t earliest = s_face_started_us + (FACE_MS - FACE_FADE_OUT_MS) * 1000LL;
        int64_t t = esp_timer_get_time();
        s_boot_fade_started_us = t > earliest ? t : earliest;
    }
    render_shell_view();
    refresh(NULL);
    if (!s_face_active && !s_boot_active && s_rendered_view == VIEW_APP &&
        !settings_overlay_open() && !s_wifi_setup_active)
        repaint_dashboard_chrome();
    lv_port_sem_give();
}

void display_usage(float five_used, const char *five_reset,
                   float week_used, const char *week_reset)
{
    provider_usage_t usage = {0};
    usage.configured = true;
    usage.has_sample = true;
    usage.sampled_at = s_clock_ready && s_time_valid ? (int64_t)time(NULL) : 0;
    usage.sampled_us = esp_timer_get_time();
    const float used[] = {five_used, week_used};
    const char *reset[] = {five_reset, week_reset};
    const uint32_t windows[] = {18000, 604800};
    for (int i = 0; i < 2; ++i) {
        usage.windows[i].valid = isfinite(used[i]) && used[i] >= 0 && used[i] <= 100;
        usage.windows[i].used_percent = used[i];
        usage.windows[i].window_seconds = windows[i];
        display_parse_reset(reset[i], &usage.windows[i].resets_at);
    }
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
}

void display_updating(void)
{
    lv_port_sem_take();
    mascot_reset();
    s_updating = true;
    if (s_boot_active) boot_status("Loading usage...");
    if (s_has_usage) {
        s_error[0] = '\0';
        refresh(NULL);
    }
    lv_port_sem_give();
}

void display_set_refresh_callback(display_refresh_callback_t callback, void *context)
{
    lv_port_sem_take();
    s_refresh_callback = callback;
    s_refresh_context = context;
    if (!callback) {
        s_refresh_pending = false;
        s_refresh_pending_mask = 0;
        s_escaped_pending_mask = 0;
    }
    lv_port_sem_give();
}

void display_set_app_refresh_callback(display_app_refresh_callback_t callback,
                                      void *context)
{
    lv_port_sem_take();
    s_app_refresh_callback = callback;
    s_app_refresh_context = context;
    if (!callback) {
        s_refresh_pending = false;
        s_refresh_pending_mask = 0;
        s_escaped_pending_mask = 0;
    }
    lv_port_sem_give();
}

void display_app_refresh_begin(app_id_t id, uint32_t generation)
{
    lv_port_sem_take();
    bool escaped = id > APP_ID_NONE && id < 32 &&
                   (s_escaped_pending_mask & (1u << id)) != 0;
    if (id > APP_ID_NONE && id < 32)
        s_refresh_pending_mask &= ~(1u << id);
    s_refresh_pending = s_refresh_pending_mask != 0;
    ui_shell_refresh_begin(&s_shell, id, generation, esp_timer_get_time());
    if (escaped) {
        s_escaped_pending_mask &= ~(1u << id);
        ui_shell_navigate_saver(&s_shell, SAVER_ID_WORD_CLOCK,
                                esp_timer_get_time());
    }
    s_cycle_inflight = ui_shell_is_refreshing(&s_shell, APP_ID_ALL);
    if (!escaped && box_config_current()->refresh_animation && !s_face_active &&
        !s_boot_active && !s_setup_visible && !s_settings_active &&
        !s_web_setup_active && !s_wifi_setup_active) {
        ui_shell_set_face(&s_shell, true, esp_timer_get_time());
        if (ui_shell_view(&s_shell).overlay == UI_OVERLAY_FACE) start_face();
    }
    refresh(NULL);
    lv_port_sem_give();
}

void display_app_refresh_end(app_id_t id, uint32_t generation)
{
    lv_port_sem_take();
    ui_shell_refresh_end(&s_shell, id, generation, esp_timer_get_time());
    s_cycle_inflight = ui_shell_is_refreshing(&s_shell, APP_ID_ALL);
    s_refresh_pending = s_refresh_pending_mask != 0;
    if (!s_cycle_inflight) s_updating = false;
    refresh(NULL);
    lv_port_sem_give();
}

void display_refresh_cycle_begin(void)
{
    static uint32_t legacy_generation;
    if (++legacy_generation == 0) ++legacy_generation;
    display_app_refresh_begin(APP_ID_CLAUDE, legacy_generation);
}

void display_refresh_cycle_end(void)
{
    /* Legacy callers have a single Claude cycle; its generation is current. */
    display_app_refresh_end(APP_ID_CLAUDE,
        s_shell.app_state[USAGE_PROVIDER_CLAUDE].generation);
}

void display_clock_ready(void)
{
    lv_port_sem_take();
    s_clock_ready = true;
    refresh(NULL);
    lv_port_sem_give();
}

const app_registry_t *display_app_registry(void)
{
    return &s_apps;
}

const screensaver_registry_t *display_screensaver_registry(void)
{
    return &s_savers;
}
