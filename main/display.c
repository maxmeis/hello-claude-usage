#include "display.h"
#include "display_format.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp_board.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "lv_port.h"
#include "lvgl.h"
#include "usage_fonts.h"

#define BG 0x000000
#define CARD 0x252B27
#define TEXT 0xF9F6EF
#define MUTED 0xDDDCD6
#define ORANGE 0xD97757
#define TRACK 0x655A64
#define PILL 0x5C515C
#define WEEK 0xB3C84D
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
#define MASCOT_X 24
#define MASCOT_Y 24
#define MASCOT_TICK_MS 150
#define MASCOT_CYCLE_MS 8000
#define MASCOT_PLAY_START_MS 6300

typedef struct {
    lv_obj_t *value;
    lv_obj_t *fill;
    lv_obj_t *reset;
    char reset_iso[64];
} card_t;

static lv_obj_t *s_content;
static lv_obj_t *s_mascot, *s_mascot_left_foot, *s_mascot_right_foot;
static lv_obj_t *s_clock;
static lv_obj_t *s_footer;
static lv_obj_t *s_refresh_hitbox;
static lv_obj_t *s_face, *s_face_left_eye, *s_face_right_eye, *s_boot_label;
static lv_timer_t *s_face_timer;
static card_t s_five, s_week;
static bool s_clock_ready, s_has_usage, s_updating, s_setup_visible;
static bool s_face_active, s_refresh_pending, s_cycle_inflight;
static bool s_boot_active, s_boot_ready;
static int64_t s_face_started_us;
static int64_t s_boot_fade_started_us;
static int64_t s_mascot_cycle_started_us;
static int8_t s_mascot_frame = -1;
static display_refresh_callback_t s_refresh_callback;
static void *s_refresh_context;
static int64_t s_measured_us;
static char s_error[72];
static const char *s_status_prefix = "Error";

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
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
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

/* Squat, square-pixel Clawd. */
static void mascot(lv_obj_t *screen)
{
    s_mascot = lv_obj_create(screen);
    lv_obj_remove_style_all(s_mascot);
    lv_obj_set_pos(s_mascot, MASCOT_X, MASCOT_Y);
    lv_obj_set_size(s_mascot, 56, 40);
    lv_obj_set_style_bg_opa(s_mascot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_mascot, 0, 0);
    lv_obj_clear_flag(s_mascot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    pixel(s_mascot, 8, 8, 36, 24, ORANGE);
    pixel(s_mascot, 14, 4, 7, 4, ORANGE);
    pixel(s_mascot, 31, 4, 7, 4, ORANGE);
    pixel(s_mascot, 4, 14, 4, 16, ORANGE);
    pixel(s_mascot, 44, 14, 4, 16, ORANGE);
    s_mascot_left_foot = pixel(s_mascot, 14, 32, 6, 5, ORANGE);
    pixel(s_mascot, 23, 32, 6, 5, ORANGE);
    s_mascot_right_foot = pixel(s_mascot, 32, 32, 6, 5, ORANGE);
    pixel(s_mascot, 18, 16, 4, 5, BG);
    pixel(s_mascot, 30, 16, 4, 5, BG);
}

typedef struct {
    int8_t x, y;
    int8_t lifted_foot; /* -1 left, +1 right, 0 neither */
} mascot_frame_t;

/* Six and a half seconds still, then a 1.5 s stepped shuffle. */
static const mascot_frame_t s_mascot_frames[] = {
    {1, 0, -1}, {2, 0, 1}, {3, 1, -1}, {4, 1, 1}, {4, 2, -1},
    {3, 1, 1}, {2, 1, -1}, {1, 0, 1}, {0, 0, -1}, {0, 0, 0},
};

static void mascot_reset(void)
{
    s_mascot_cycle_started_us = 0;
    if (s_mascot_frame == -1) return;
    lv_obj_set_pos(s_mascot, MASCOT_X, MASCOT_Y);
    lv_obj_set_y(s_mascot_left_foot, 32);
    lv_obj_set_y(s_mascot_right_foot, 32);
    s_mascot_frame = -1;
}

/* LVGL timer callbacks run under the port lock. No objects are created here. */
static void mascot_tick(lv_timer_t *timer)
{
    (void)timer;
    int64_t now = esp_timer_get_time();
    int64_t age = (now - s_measured_us) / 1000000;
    if (!s_has_usage || s_setup_visible || s_updating || s_error[0] ||
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
    lv_obj_set_pos(s_mascot, MASCOT_X + pose->x, MASCOT_Y + pose->y);
    lv_obj_set_y(s_mascot_left_foot, pose->lifted_foot < 0 ? 31 : 32);
    lv_obj_set_y(s_mascot_right_foot, pose->lifted_foot > 0 ? 31 : 32);
    s_mascot_frame = frame;
}

static void face_eye(lv_obj_t *eye, int height)
{
    lv_obj_set_y(eye, 250 - height / 2);
    lv_obj_set_height(eye, height);
}

static uint32_t clock_face_color(uint8_t opacity)
{
    const uint32_t from = TEXT, to = FACE_INK;
    uint32_t r = (((from >> 16) & 255) * (255 - opacity) +
                  ((to >> 16) & 255) * opacity) / 255;
    uint32_t g = (((from >> 8) & 255) * (255 - opacity) +
                  ((to >> 8) & 255) * opacity) / 255;
    uint32_t b = ((from & 255) * (255 - opacity) +
                  (to & 255) * opacity) / 255;
    return (r << 16) | (g << 8) | b;
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

static void stop_face(void)
{
    s_face_active = false;
    lv_timer_pause(s_face_timer);
    lv_obj_add_flag(s_face, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(TEXT), 0);
    face_eye(s_face_left_eye, FACE_EYE_H);
    face_eye(s_face_right_eye, FACE_EYE_H);
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
    lv_obj_set_style_text_color(s_clock, lv_color_hex(clock_face_color(opacity)), 0);
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
    lv_obj_move_foreground(s_clock);
    lv_timer_reset(s_face_timer);
    lv_timer_resume(s_face_timer);
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
    lv_obj_clean(s_content);
    memset(&s_five, 0, sizeof(s_five));
    memset(&s_week, 0, sizeof(s_week));
}

static void footer(const char *message)
{
    /* Long failures keep their retry detail; ordinary updates stay on one line. */
    lv_obj_set_y(s_footer, strlen(message) > 48 ? 409 : 429);
    lv_label_set_text(s_footer, message);
    lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
}

static void footer_clicked(lv_event_t *event)
{
    (void)event;
    if (!s_has_usage || s_setup_visible || s_updating || s_cycle_inflight ||
        s_refresh_pending || s_face_active || !s_refresh_callback) return;
    s_refresh_pending = true;
    if (!s_refresh_callback(s_refresh_context)) s_refresh_pending = false;
    else mascot_reset();
}

/* LVGL timer callbacks run on the LVGL task, under the port's own lock. */
static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (s_clock_ready) {
        time_t now = time(NULL);
        struct tm local;
        if (localtime_r(&now, &local) && local.tm_year >= 125) {
            char clock[6];
            strftime(clock, sizeof(clock), "%H:%M", &local);
            lv_label_set_text(s_clock, clock);
        } else {
            lv_label_set_text(s_clock, "--:--");
        }
    } else {
        lv_label_set_text(s_clock, "--:--");
    }
    if (!s_has_usage) return;

    int64_t age = (esp_timer_get_time() - s_measured_us) / 1000000;
    if (age < 0) age = 0;
    bool stale = age > STALE_SEC;
    lv_obj_set_style_bg_opa(s_five.fill, stale ? LV_OPA_50 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_week.fill, stale ? LV_OPA_50 : LV_OPA_COVER, 0);
    char message[96];
    if (s_error[0]) {
        snprintf(message, sizeof(message), "%s: %s", s_status_prefix, s_error);
    } else if (s_updating) {
        snprintf(message, sizeof(message), "Updating: last update %lldm ago", (long long)(age / 60));
    } else if (stale) {
        snprintf(message, sizeof(message), "Stale: updated %lldm ago", (long long)(age / 60));
    } else if (age < 60) {
        snprintf(message, sizeof(message), "Updated %llds ago", (long long)age);
    } else {
        snprintf(message, sizeof(message), "Updated %lldm ago", (long long)(age / 60));
    }
    footer(message);

    char reset[64];
    time_t now = time(NULL);
    display_format_reset(reset, sizeof(reset), s_five.reset_iso, now, s_clock_ready);
    lv_label_set_text(s_five.reset, reset);
    display_format_reset(reset, sizeof(reset), s_week.reset_iso, now, s_clock_ready);
    lv_label_set_text(s_week.reset, reset);
}

static void status(const char *title, const char *detail)
{
    clear_content();
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_t *box = rect(s_content, 0, y, W, CARD_H, CARD, 5);
    c.value = label(box, &usage_values_font, TEXT, CARD_PAD, 7, 240, "--%");
    lv_obj_t *pill = rect(box, 284, 15, 132, 34, PILL, 5);
    lv_obj_t *name = label(pill, &usage_ui_font, TEXT, 0, 4, 132, period);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    rect(box, CARD_PAD, 66, BAR_W, 14, TRACK, 3);
    c.fill = rect(box, CARD_PAD, 66, 0, 14, color, 3);
    c.reset = label(box, &usage_ui_font, MUTED, CARD_PAD, 94, BAR_W, "Reset unknown");
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
    ESP_ERROR_CHECK(bsp_board_init());
    lv_port_init();
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    lv_port_sem_take();
    lv_obj_t *screen = lv_scr_act();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    mascot(screen);
    lv_obj_t *heading = label(screen, &usage_heading_font, TEXT, 139, 25, 200, "Usage");
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    s_clock = label(screen, &usage_ui_font, TEXT, 350, 32, 106, "--:--");
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, 0);

    s_content = lv_obj_create(screen);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_pos(s_content, 24, 106);
    lv_obj_set_size(s_content, W, 344);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_outline_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    s_footer = label(screen, &usage_status_font, ORANGE, 24, 429, W, "");
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_footer, 51);
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    s_refresh_hitbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_refresh_hitbox);
    lv_obj_set_pos(s_refresh_hitbox, 24, 405);
    lv_obj_set_size(s_refresh_hitbox, W, 51);
    lv_obj_clear_flag(s_refresh_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_refresh_hitbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_refresh_hitbox, footer_clicked, LV_EVENT_CLICKED, NULL);

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
    lv_obj_set_style_text_color(s_clock, lv_color_hex(FACE_INK), 0);
    lv_obj_move_foreground(s_clock);
    status("Starting", "Connecting to Claude...");
    lv_timer_create(refresh, 1000, NULL);
    lv_timer_create(mascot_tick, MASCOT_TICK_MS, NULL);
    s_face_timer = lv_timer_create(face_tick, FACE_TICK_MS, NULL);
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
    bool refreshing = detail && strstr(detail, "Refreshing token");
    bool waiting_for_time = (title && strcmp(title, "Time") == 0) ||
                            (detail && strstr(detail, "Waiting for clock"));
    bool usage_retry = title && strcmp(title, "Usage") == 0 && detail &&
                       strstr(detail, "Retrying after cooldown");
    if (!refreshing) {
        s_updating = false;
        s_refresh_pending = false;
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
    s_updating = false;
    s_setup_visible = true;
    s_refresh_pending = false;
    s_cycle_inflight = false;
    stop_face();
    if (!url || !*url) {
        status("Connect Claude", "Setup link unavailable. Check Wi-Fi.");
        lv_port_sem_give();
        return;
    }
    clear_content();
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    label(s_content, &usage_ui_font, TEXT, 0, 0, W, "Connect Claude");
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

void display_usage(float five_used, const char *five_reset,
                   float week_used, const char *week_reset)
{
    lv_port_sem_take();
    if (!s_has_usage) {
        clear_content();
        s_five = create_card(0, "5h left", ORANGE);
        s_week = create_card(CARD_H + CARD_GAP, "Week left", WEEK);
    }
    update_card(&s_five, five_used, five_reset);
    update_card(&s_week, week_used, week_reset);
    s_measured_us = esp_timer_get_time();
    s_has_usage = true;
    s_setup_visible = false;
    s_updating = false;
    s_error[0] = '\0';
    if (s_boot_active && !s_boot_ready) {
        s_boot_ready = true;
        int64_t earliest = s_face_started_us + (FACE_MS - FACE_FADE_OUT_MS) * 1000LL;
        int64_t now = esp_timer_get_time();
        s_boot_fade_started_us = now > earliest ? now : earliest;
    }
    refresh(NULL);
    lv_port_sem_give();
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
    if (!callback) s_refresh_pending = false;
    lv_port_sem_give();
}

void display_refresh_cycle_begin(void)
{
    lv_port_sem_take();
    mascot_reset();
    s_refresh_pending = false;
    if (!s_cycle_inflight) {
        s_cycle_inflight = true;
        if (s_has_usage && !s_setup_visible && !s_face_active) start_face();
    }
    lv_port_sem_give();
}

void display_refresh_cycle_end(void)
{
    lv_port_sem_take();
    s_refresh_pending = false;
    s_cycle_inflight = false;
    s_updating = false;
    if (s_has_usage) refresh(NULL);
    lv_port_sem_give();
}

void display_clock_ready(void)
{
    lv_port_sem_take();
    s_clock_ready = true;
    refresh(NULL);
    lv_port_sem_give();
}
