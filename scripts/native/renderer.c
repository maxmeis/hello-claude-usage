#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <assert.h>

#include "display.h"
#include "display_format.h"
#include "usage_provider.h"
#include "word_clock.h"
#include "box_config.h"
#include "app_registry.h"
#include "screensaver_registry.h"
#include "ui_shell.h"
#include "lvgl.h"
#include "usage_fonts.h"

#define WIDTH 480
#define HEIGHT 480
static lv_color_t frame[WIDTH * HEIGHT];
static lv_color_t draw_buffer[WIDTH * 40];
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t display_driver;
static lv_indev_drv_t pointer_driver;
static lv_point_t pointer_point;
static bool pointer_pressed;
static unsigned refresh_requests;
static bool refresh_accept = true;
typedef struct {
    unsigned calls;
    app_id_t targets[16];
    uint32_t accepted_mask;
} app_refresh_log_t;
static app_refresh_log_t app_refresh_log;
static bool settings_accept = true;
static bool settings_close_accept = true;
static unsigned settings_opens, settings_closes;
static uint32_t last_settings_id, last_closed_id;
static int64_t fake_us = 1000000;
static uint32_t scenario_elapsed_ms;
static unsigned board_assert_count;
static unsigned brightness_set_calls;
static uint8_t brightness_percent;
static time_t mock_epoch = 1761500000;

static int write_ppm(const char *path);
static void pointer_tap(int x, int y);
static provider_usage_t example_provider(float first, float second,
                                         uint32_t first_window, uint32_t second_window);
static int contains_label(lv_obj_t *object, const char *prefix);
static lv_obj_t *find_label(lv_obj_t *object, const char *text);
static lv_obj_t *find_label_prefix(lv_obj_t *object, const char *prefix);
static lv_obj_t *find_descendant_box(lv_obj_t *object, int x, int y, int width, int height);
static int global_object_x(lv_obj_t *object);
static int global_object_y(lv_obj_t *object);
static void assert_header_targets(bool visible);
static void assert_usage_footer(void);
static void assert_age_label(const char *text);
static void assert_clock_view(void);
static void assert_board_view(void);
static void assert_clock_mascot_hidden(void);
static void assert_word_render(int hour, int minute, bool ready);
static void assert_no_visible_digital_clock(void);
static void assert_overlay_qr(lv_obj_t *overlay, bool shown);
static lv_obj_t *find_clock(lv_obj_t *object);
static lv_obj_t *word_layer(void);
static lv_obj_t *overlay_for_title(const char *title);

static void mascot_motion_scene(const char *base);
static void mascot_pause_scene(const char *scene, const char *base);
static void provider_scene(const char *scene, const char *output);
static void shell_policy_scene(const char *output);
static void set_berlin_time(int hour, int minute);

int64_t esp_timer_get_time(void) { return fake_us; }
time_t time(time_t *out)
{
    if (out) *out = mock_epoch;
    return mock_epoch;
}
void lv_port_sem_take(void) {}
void lv_port_sem_give(void) {}
void lv_port_set_brightness(uint8_t percent)
{
    ++brightness_set_calls;
    brightness_percent = percent;
}

static void pointer_read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    data->point = pointer_point;
    data->state = pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static bool mock_refresh(void *context)
{
    assert(context == &refresh_requests);
    ++refresh_requests;
    return refresh_accept;
}

static uint32_t mock_app_refresh(void *context, app_id_t id)
{
    app_refresh_log_t *log = context;
    assert(log == &app_refresh_log);
    assert(log->calls < sizeof(log->targets) / sizeof(log->targets[0]));
    log->targets[log->calls++] = id;
    return log->accepted_mask;
}

static bool mock_settings(void *context, uint32_t request_id, bool open)
{
    assert(context == &settings_opens);
    if (open) {
        ++settings_opens;
        last_settings_id = request_id;
        return settings_accept;
    }
    ++settings_closes;
    last_closed_id = request_id;
    return settings_close_accept;
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    for (int y = area->y1; y <= area->y2; y++) {
        if (y < 0 || y >= HEIGHT) continue;
        int x1 = area->x1 < 0 ? 0 : area->x1;
        int x2 = area->x2 >= WIDTH ? WIDTH - 1 : area->x2;
        if (x1 <= x2) memcpy(&frame[y * WIDTH + x1],
                             &colors[(y - area->y1) * (area->x2 - area->x1 + 1) + x1 - area->x1],
                             (size_t)(x2 - x1 + 1) * sizeof(lv_color_t));
    }
    lv_disp_flush_ready(driver);
}

void lv_port_init(void)
{
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, draw_buffer, NULL, WIDTH * 40);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = WIDTH;
    display_driver.ver_res = HEIGHT;
    display_driver.flush_cb = flush;
    display_driver.draw_buf = &draw_buf;
    lv_disp_drv_register(&display_driver);
    lv_indev_drv_init(&pointer_driver);
    pointer_driver.type = LV_INDEV_TYPE_POINTER;
    pointer_driver.read_cb = pointer_read;
    lv_indev_drv_register(&pointer_driver);
}

static void reset_iso(char *buffer, size_t length, time_t offset)
{
    time_t when = time(NULL) + offset;
    struct tm utc;
    gmtime_r(&when, &utc);
    strftime(buffer, length, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static void step_ms(uint32_t elapsed_ms)
{
    fake_us += (int64_t)elapsed_ms * 1000;
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
    lv_refr_now(NULL);
}

static void step_periodic_ms(uint32_t elapsed_ms)
{
    while (elapsed_ms) {
        uint32_t delta = elapsed_ms > 100 ? 100 : elapsed_ms;
        step_ms(delta);
        elapsed_ms -= delta;
    }
}

static int pixel_is_orange(int x, int y)
{
    return frame[y * WIDTH + x].full == lv_color_hex(0xD97757).full;
}

static int pixel_is_eye_dark(int x, int y)
{
    return frame[y * WIDTH + x].full == lv_color_hex(0x000000).full;
}

static void assert_black_corners(void)
{
    const int corner_x[] = {0, WIDTH - 1};
    const int corner_y[] = {0, HEIGHT - 1};
    for (size_t yi = 0; yi < 2; ++yi)
        for (size_t xi = 0; xi < 2; ++xi)
            assert(frame[corner_y[yi] * WIDTH + corner_x[xi]].full ==
                   lv_color_hex(0x000000).full && "screen corners must stay pure black in RGB565");
}

static lv_obj_t *find_screen_box(int x, int y, int width, int height)
{
    lv_obj_t *screen = lv_scr_act();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (lv_obj_get_x(child) == x && lv_obj_get_y(child) == y &&
            lv_obj_get_width(child) == width && lv_obj_get_height(child) == height)
            return child;
    }
    return NULL;
}

static void open_claude_detail(void)
{
    lv_obj_t *content = find_screen_box(24, 90, 432, 344);
    if (content && !lv_obj_has_flag(content, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_t *home = find_screen_box(0, 56, 480, 424);
    if (!home || lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN)) {
        /* The shell may have returned to its saver after a long idle or stale interval. */
        pointer_tap(200, 200);
        home = find_screen_box(0, 56, 480, 424);
    }
    assert(home && !lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN) &&
           "legacy Claude dashboard capture starts from the registered Home widgets");
    pointer_tap(240, 160);
    content = find_screen_box(24, 90, 432, 344);
    assert(content && !lv_obj_has_flag(content, LV_OBJ_FLAG_HIDDEN) &&
           "configured Claude widget opens its provider detail view");
    lv_refr_now(NULL);
}

static unsigned count_pixel_color(int x1, int y1, int x2, int y2, uint32_t hex)
{
    unsigned count = 0;
    lv_color_t wanted = lv_color_hex(hex);
    for (int y = y1; y < y2; ++y)
        for (int x = x1; x < x2; ++x)
            count += frame[y * WIDTH + x].full == wanted.full;
    return count;
}

static void assert_shell_header_ink(bool home_route)
{
    lv_refr_now(NULL);
    lv_obj_t *home_icon = find_label(lv_scr_act(), LV_SYMBOL_HOME);
    lv_obj_t *refresh_icon = find_label(lv_scr_act(), LV_SYMBOL_REFRESH);
    lv_obj_t *settings_icon = find_label(lv_scr_act(), LV_SYMBOL_SETTINGS);
    assert(refresh_icon && settings_icon &&
           "the shared header renders Refresh and Settings symbols");
    assert(lv_obj_get_style_text_font(refresh_icon, LV_PART_MAIN) == &lv_font_montserrat_24 &&
           lv_obj_get_style_text_font(settings_icon, LV_PART_MAIN) == &lv_font_montserrat_24 &&
           "header symbols use the enabled 24px Montserrat font");
    if (home_route) {
        assert(!home_icon || lv_obj_has_flag(home_icon, LV_OBJ_FLAG_HIDDEN));
        assert(count_pixel_color(16, 0, 96, 56, 0xD97757) > 40 &&
               "Home header retains its Clawd target");
        assert(count_pixel_color(96, 0, 264, 56, 0xF9F6EF) > 20 &&
               "Home title is visible in its header slot");
    } else {
        assert(home_icon && !lv_obj_has_flag(home_icon, LV_OBJ_FLAG_HIDDEN) &&
               lv_obj_get_style_text_font(home_icon, LV_PART_MAIN) == &lv_font_montserrat_24 &&
               "app routes show the 24px Home symbol");
        assert(count_pixel_color(8, 0, 56, 56, 0xF9F6EF) > 8 &&
               count_pixel_color(60, 0, 116, 56, 0xD97757) > 40 &&
               "app header shows Home and Clawd actions");
        assert(count_pixel_color(128, 0, 264, 56, 0xF9F6EF) > 20 &&
               "app title is visible in the shared header");
    }
    assert_no_visible_digital_clock();
    assert(count_pixel_color(280, 0, 352, 56, 0xF9F6EF) == 0 &&
           count_pixel_color(356, 0, 404, 56, 0xF9F6EF) > 5 &&
           count_pixel_color(408, 0, 456, 56, 0xF9F6EF) > 8 &&
           "Refresh and Settings remain visible beside an empty clock area");
}

static void assert_home_widget_pixels(void)
{
    lv_obj_t *home = find_screen_box(0, 56, 480, 424);
    assert(home && !lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN) &&
           "Home must show the registered provider widgets");
    assert(lv_obj_get_child_cnt(home) >= 2 &&
           "both registered providers have a Home widget slot");
    const int slot_y[] = {64, 272};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *slot = find_descendant_box(home, 8, slot_y[i], 464, 200);
        assert(slot && !lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN) &&
               lv_obj_get_child_cnt(slot) > 0 &&
               "Home provider rows resolve to their 464x200 layout before mounting");
        lv_obj_t *widget = lv_obj_get_child(slot, 0);
        assert(lv_obj_get_width(widget) == 464 && lv_obj_get_height(widget) == 200 &&
               "registered widget root receives the resolved slot bounds");
        assert(lv_obj_get_style_border_width(widget, LV_PART_MAIN) == 0 &&
               lv_obj_get_style_radius(widget, LV_PART_MAIN) == 0 &&
               "Home provider rows have no rounded card outlines");
    }
    assert(contains_label(home, "Claude") && contains_label(home, "Codex"));
    lv_obj_t *claude = find_descendant_box(home, 8, 64, 464, 200);
    lv_obj_t *codex = find_descendant_box(home, 8, 272, 464, 200);
    lv_obj_t *claude_value = find_label(claude, "50%");
    lv_obj_t *codex_value = find_label(codex, "80%");
    lv_obj_t *claude_period = find_label(claude, "5h left");
    lv_obj_t *codex_period = find_label(codex, "Week left");
    lv_obj_t *claude_countdown = find_label_prefix(claude, "Resets in");
    lv_obj_t *codex_countdown = find_label_prefix(codex, "Resets in");
    lv_obj_t *codex_setup = find_label(codex, "Set up Codex");
    lv_obj_t *codex_unavailable = find_label(codex, "Weekly usage unavailable");
    const bool codex_needs_setup = codex_setup &&
                                   !lv_obj_has_flag(codex_setup, LV_OBJ_FLAG_HIDDEN);
    const bool codex_weekly_missing = codex_unavailable &&
                                      !lv_obj_has_flag(codex_unavailable, LV_OBJ_FLAG_HIDDEN);
    if (codex_needs_setup || codex_weekly_missing) {
        assert(!codex_value || lv_obj_has_flag(codex_value, LV_OBJ_FLAG_HIDDEN));
        assert(!codex_countdown || lv_obj_has_flag(codex_countdown, LV_OBJ_FLAG_HIDDEN));
        assert(!find_label(codex, "Reset tokens: 3") ||
               lv_obj_has_flag(find_label(codex, "Reset tokens: 3"), LV_OBJ_FLAG_HIDDEN));
    }
    if (claude_value && !lv_obj_has_flag(claude_value, LV_OBJ_FLAG_HIDDEN))
        assert(lv_obj_get_style_text_font(claude_value, LV_PART_MAIN) == &usage_values_font);
    if (codex_value && !lv_obj_has_flag(codex_value, LV_OBJ_FLAG_HIDDEN))
        assert(lv_obj_get_style_text_font(codex_value, LV_PART_MAIN) == &usage_values_font);
    if (claude_period && !lv_obj_has_flag(claude_period, LV_OBJ_FLAG_HIDDEN))
        assert(lv_obj_get_style_text_font(claude_period, LV_PART_MAIN) == &usage_status_font);
    if (codex_countdown && !lv_obj_has_flag(codex_countdown, LV_OBJ_FLAG_HIDDEN))
        assert(lv_obj_get_style_text_font(codex_countdown, LV_PART_MAIN) == &usage_status_font);
    lv_obj_t *codex_tokens = find_label_prefix(codex, "Reset tokens:");
    if (codex_tokens && !lv_obj_has_flag(codex_tokens, LV_OBJ_FLAG_HIDDEN))
        assert(lv_obj_get_style_text_font(codex_tokens, LV_PART_MAIN) == &usage_meta_font &&
               "known Codex reset-token counts use the compact metadata font");
    assert(frame[64 * WIDTH + 8].full == lv_color_hex(0x000000).full &&
           frame[64 * WIDTH + 471].full == lv_color_hex(0x000000).full &&
           frame[272 * WIDTH + 8].full == lv_color_hex(0x000000).full &&
           frame[272 * WIDTH + 471].full == lv_color_hex(0x000000).full &&
           "clean Home rows have no rounded outline pixels");
    lv_refr_now(NULL);
    assert(frame[268 * WIDTH + 240].full == lv_color_hex(0x000000).full &&
           "the gap between stacked app rows stays black");
}

static void assert_app_detail_pixels(int assert_quota_palette)
{
    lv_obj_t *content = find_screen_box(24, 90, 432, 344);
    assert(content && !lv_obj_has_flag(content, LV_OBJ_FLAG_HIDDEN) &&
           "an app route must show its two-window detail content");
    const lv_color_t black = lv_color_hex(0x000000);
    const lv_color_t reset_ink = lv_color_hex(0xDDDCD6);
    const int samples[][2] = {{450, 215}, {450, 365}, {240, 230}};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        assert(frame[samples[i][1] * WIDTH + samples[i][0]].full == black.full &&
               "detail rows have no gray cards or unused bar tracks");
    }
    assert(frame[198 * WIDTH + 48].full != black.full &&
           frame[348 * WIDTH + 48].full != black.full &&
           "detail resets retain their visible clock glyph");
    if (assert_quota_palette) {
        assert(frame[168 * WIDTH + 80].full ==
               lv_color_hex(display_quota_color(50.0f)).full &&
               "first detail quota bar paints its actual remaining color");
        assert(frame[318 * WIDTH + 60].full ==
               lv_color_hex(display_quota_color(11.0f)).full &&
               "second detail quota bar paints its actual remaining color");
        assert(frame[168 * WIDTH + 280].full == black.full &&
               frame[318 * WIDTH + 100].full == black.full &&
               "unfilled quota space remains pure black");
        assert(frame[180 * WIDTH + 80].full == black.full &&
               frame[330 * WIDTH + 60].full == black.full &&
               "24-pixel fills stop before their reset labels");
    }
}

static void assert_flat_dashboard_pixels(int assert_quota_palette)
{
    lv_obj_t *home = find_screen_box(0, 56, 480, 424);
    if (home && !lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN)) {
        assert_home_widget_pixels();
        return;
    }
    assert_app_detail_pixels(assert_quota_palette);
}

static void assert_stale_fill_pixels(void)
{
    const lv_color_t black = lv_color_hex(0x000000);
    const int samples[][2] = {{100, 166}, {60, 316}};
    const float remaining[] = {50.0f, 11.0f};
    for (size_t i = 0; i < 2; ++i) {
        lv_color_t quota = lv_color_hex(display_quota_color(remaining[i]));
        lv_color_t faded = lv_color_mix(quota, black, LV_OPA_50);
        lv_color_t actual = frame[samples[i][1] * WIDTH + samples[i][0]];
        lv_color32_t actual_rgb, faded_rgb;
        actual_rgb.full = lv_color_to32(actual);
        faded_rgb.full = lv_color_to32(faded);
        int red_delta = (int)actual_rgb.ch.red - (int)faded_rgb.ch.red;
        int green_delta = (int)actual_rgb.ch.green - (int)faded_rgb.ch.green;
        int blue_delta = (int)actual_rgb.ch.blue - (int)faded_rgb.ch.blue;
        assert(abs(red_delta) <= 12 && abs(green_delta) <= 12 && abs(blue_delta) <= 12 &&
               "stale progress fill must remain visible at half opacity over black");
        assert(actual.full != black.full &&
               "stale progress fill must not disappear into the black background");
    }
    assert(frame[166 * WIDTH + 250].full == black.full &&
           frame[316 * WIDTH + 100].full == black.full);
}

static void assert_face_pixel(int x, int y, uint32_t color)
{
    if (frame[y * WIDTH + x].full != lv_color_hex(color).full) {
        fprintf(stderr, "face pixel (%d,%d): got 0x%04x expected 0x%04x\n", x, y,
                (unsigned)frame[y * WIDTH + x].full,
                (unsigned)lv_color_hex(color).full);
        abort();
    }
}

static void assert_face_open(void)
{
    assert_face_pixel(10, 10, 0xD97757);
    assert_face_pixel(160, 190, 0x151A17);
    assert_face_pixel(320, 190, 0x151A17);
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_face_pixel(135, 250, 0xD97757);
    assert_face_pixel(184, 250, 0xD97757);
    assert_face_pixel(295, 250, 0xD97757);
    assert_face_pixel(344, 250, 0xD97757);
    assert_no_visible_digital_clock();
}

static void assert_face_closed(void)
{
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_face_pixel(160, 190, 0xD97757);
    assert_face_pixel(320, 190, 0xD97757);
    assert_no_visible_digital_clock();
}

static void pointer_tap(int x, int y)
{
    pointer_point.x = x;
    pointer_point.y = y;
    pointer_pressed = true;
    step_ms(40);
    pointer_pressed = false;
    step_ms(40);
    scenario_elapsed_ms += 80;
}

static void pointer_gesture(int press_x, int press_y, int release_x, int release_y)
{
    pointer_point.x = press_x;
    pointer_point.y = press_y;
    pointer_pressed = true;
    step_ms(40);
    pointer_point.x = release_x;
    pointer_point.y = release_y;
    pointer_pressed = false;
    step_ms(40);
    scenario_elapsed_ms += 80;
}

static void save_ppm(const char *path)
{
    assert(write_ppm(path) == 0);
}

static void make_frame_path(char *out, size_t size, const char *base, const char *phase)
{
    size_t length = strlen(base);
    const char *extension = strrchr(base, '.');
    if (extension && strcmp(extension, ".ppm") == 0) length = (size_t)(extension - base);
    assert(snprintf(out, size, "%.*s-%s.ppm", (int)length, base, phase) < (int)size);
}

static void step_until(uint32_t target_ms)
{
    while (scenario_elapsed_ms < target_ms) {
        step_ms(20);
        scenario_elapsed_ms += 20;
    }
    assert(scenario_elapsed_ms == target_ms);
}

static void face_refresh(const char *base, const char *five_reset, const char *week_reset)
{
    box_settings_t settings = *box_config_current();
    settings.refresh_animation = true;
    box_config_set_current(&settings);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    open_claude_detail();
    lv_refr_now(NULL);
    assert_black_corners();
    assert_flat_dashboard_pixels(1);
    memset(&app_refresh_log, 0, sizeof(app_refresh_log));
    display_set_app_refresh_callback(mock_app_refresh, &app_refresh_log);
    lv_refr_now(NULL);
    assert_no_visible_digital_clock();

    /* Actual LVGL pointer events target the shared Update hitbox, including its edges. */
    app_refresh_log.accepted_mask = 0;
    pointer_tap(356, 0);
    assert(app_refresh_log.calls == 1 && "Update hitbox includes its top-left bound");
    app_refresh_log.accepted_mask = 1u << APP_ID_CLAUDE;
    pointer_tap(403, 55);
    assert(app_refresh_log.calls == 2 && "rejected callback must leave tap retryable");
    pointer_tap(380, 20);
    assert(app_refresh_log.calls == 2 && "accepted tap remains pending until cycle begins");
    display_app_refresh_begin(APP_ID_CLAUDE, 1);
    lv_refr_now(NULL);
    assert(frame[10 * WIDTH + 10].full != lv_color_hex(0x000000).full &&
           "accepted app cycle starts the face fade-in");
    assert(app_refresh_log.calls == 2);
    /* The accepted tap starts a fade; anchor the remaining phase assertions
     * to the first fully lit frame instead of assuming pointer-event latency. */
    for (unsigned waited = 0; !pixel_is_orange(10, 10); waited += 20) {
        assert(waited < 500 && "face fade reaches full brightness promptly");
        step_ms(20);
    }
    lv_refr_now(NULL);
    assert_face_open();
    assert(contains_label(lv_scr_act(), "Updating..."));
    pointer_tap(380, 20);
    assert(app_refresh_log.calls == 2 && "Update tap is ignored while face is active");
    scenario_elapsed_ms = 0; /* Phase timing starts after the ignored pointer sample. */
    char path[512];
    make_frame_path(path, sizeof(path), base, "open");
    save_ppm(path);

    step_until(1400); /* Face elapsed ~1700 ms; both eyes are closed. */
    lv_refr_now(NULL);
    assert_face_closed();
    make_frame_path(path, sizeof(path), base, "both-closed");
    save_ppm(path);

    step_until(1540); /* Face elapsed ~1840 ms; both eyes reopen. */
    lv_refr_now(NULL);
    assert_face_open();
    step_until(2700); /* Face elapsed ~3000 ms; the left eye winks. */
    lv_refr_now(NULL);
    assert_face_pixel(160, 190, 0xD97757);
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 190, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_no_visible_digital_clock();
    make_frame_path(path, sizeof(path), base, "wink");
    save_ppm(path);

    step_until(2880); /* Face elapsed ~3180 ms; wink has finished. */
    step_until(4200); /* Face elapsed ~4600 ms; still before fade-out. */
    lv_refr_now(NULL);
    assert_face_open();
    step_until(4620); /* Face elapsed ~5000 ms; overlay has completed. */
    display_app_refresh_end(APP_ID_CLAUDE, 1);
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "face overlay is hidden after five seconds");
    make_frame_path(path, sizeof(path), base, "done");
    save_ppm(path);

    /* Updating alone must never start or restart the face. */
    display_refresh_cycle_end();
    display_updating();
    step_ms(20);
    assert(!pixel_is_orange(10, 10));

    /* Quick success ends HTTP state while the one-shot face completes. */
    display_refresh_cycle_begin();
    scenario_elapsed_ms = 0;
    display_updating();
    step_until(100);
    display_refresh_cycle_end();
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    step_until(320);
    assert_face_open();
    step_until(5000);
    assert(!pixel_is_orange(10, 10));

    /* A slow request outlives the face; Updating remains, with no replay. */
    display_refresh_cycle_begin();
    scenario_elapsed_ms = 0;
    display_updating();
    step_until(5000);
    assert(!pixel_is_orange(10, 10));
    assert(contains_label(lv_scr_act(), "Updating..."));
    step_until(7000);
    assert(!pixel_is_orange(10, 10));
    display_refresh_cycle_end();

    /* Rejected taps outside the footer, during a cycle, and during setup are ignored. */
    pointer_tap(240, 400);
    assert(refresh_requests == 2);
    display_refresh_cycle_begin();
    scenario_elapsed_ms = 0;
    pointer_tap(240, 430);
    assert(refresh_requests == 2);
    display_refresh_cycle_end();
    display_status("Usage", "Connection failed; check Wi-Fi settings and try again later");
    lv_refr_now(NULL);
    assert(contains_label(lv_scr_act(), "Error:"));
    assert_usage_footer();
    pointer_tap(240, 430);
    assert(refresh_requests == 3 && "error footer remains an allowed retry target");
    pointer_tap(240, 430);
    assert(refresh_requests == 3 && "accepted error retry remains pending");
    display_refresh_cycle_begin();
    display_status("Wi-Fi", "Connection failed; retrying");
    lv_refr_now(NULL);
    assert(contains_label(lv_scr_act(), "Offline:"));
    display_refresh_cycle_begin();
    display_setup("http://192.168.4.1/setup");
    lv_refr_now(NULL);
    pointer_tap(240, 430);
    assert(refresh_requests == 3);
    assert(contains_label(lv_scr_act(), "Connect Claude"));
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    display_refresh_cycle_begin();
    scenario_elapsed_ms = 0;
    step_until(320);
    assert_face_open();
    display_status("Usage", "Connection failed; check Wi-Fi settings and try again later");
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "error aborts face immediately");
    assert(contains_label(lv_scr_act(), "Error:"));
    assert_usage_footer();
    display_refresh_cycle_begin();
    display_setup("http://192.168.4.1/setup");
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "setup aborts face immediately");
}

static int write_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); return 1; }
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        lv_color32_t color;
        color.full = lv_color_to32(frame[i]);
        unsigned char rgb[3] = {color.ch.red, color.ch.green, color.ch.blue};
        if (fwrite(rgb, 1, 3, file) != 3) { fclose(file); return 1; }
    }
    return fclose(file) == 0 ? 0 : 1;
}

static int contains_label(lv_obj_t *object, const char *prefix)
{
    /* Long-dot labels restore their text when LVGL resolves the first layout. */
    if (object == lv_scr_act()) lv_refr_now(NULL);
    if (lv_obj_check_type(object, &lv_label_class) &&
        strncmp(lv_label_get_text(object), prefix, strlen(prefix)) == 0) return 1;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i)
        if (contains_label(lv_obj_get_child(object, i), prefix)) return 1;
    return 0;
}

static lv_obj_t *find_label(lv_obj_t *object, const char *text)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        strcmp(lv_label_get_text(object), text) == 0) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(object, i), text);
        if (found) return found;
    }
    return NULL;
}

static int global_object_x(lv_obj_t *object)
{
    lv_obj_t *parent = lv_obj_get_parent(object);
    return lv_obj_get_x(object) + (parent ? global_object_x(parent) : 0);
}

static int global_object_y(lv_obj_t *object)
{
    lv_obj_t *parent = lv_obj_get_parent(object);
    return lv_obj_get_y(object) + (parent ? global_object_y(parent) : 0);
}

static lv_obj_t *find_descendant_box(lv_obj_t *object, int x, int y,
                                     int width, int height)
{
    if (global_object_x(object) == x && global_object_y(object) == y &&
        lv_obj_get_width(object) == width && lv_obj_get_height(object) == height)
        return object;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i) {
        lv_obj_t *found = find_descendant_box(lv_obj_get_child(object, i),
                                              x, y, width, height);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_label_prefix(lv_obj_t *object, const char *prefix)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        strncmp(lv_label_get_text(object), prefix, strlen(prefix)) == 0) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i) {
        lv_obj_t *found = find_label_prefix(lv_obj_get_child(object, i), prefix);
        if (found) return found;
    }
    return NULL;
}

static void assert_boot_label(const char *text)
{
    assert_no_visible_digital_clock();
    lv_obj_t *label = find_label(lv_scr_act(), text);
    assert(label && "boot status must be visible in the production LVGL tree");
    assert(lv_obj_get_style_text_font(label, LV_PART_MAIN) == &usage_status_font &&
           "boot status must use the compact production status font");
    assert(lv_obj_get_x(label) == 24 && lv_obj_get_y(label) == 366);
    assert(lv_obj_get_width(label) == 432 && lv_obj_get_height(label) == 52);
}

static void assert_face_eyes_open(void)
{
    assert_face_pixel(160, 190, 0x151A17);
    assert_face_pixel(320, 190, 0x151A17);
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
}

static void save_named_frame(const char *base, const char *phase)
{
    char path[512];
    make_frame_path(path, sizeof(path), base, phase);
    save_ppm(path);
}

static void assert_forecast_pixels(int row, display_forecast_t expected)
{
    const int x0 = 288, y0 = row == 0 ? 114 : 264;
    const lv_color_t green = lv_color_hex(0x89B18B);
    const lv_color_t orange = lv_color_hex(0xD97757);
    const lv_color_t neutral = lv_color_hex(0x777570);
    unsigned counts[3] = {0};
    lv_refr_now(NULL);
    for (int y = y0 - 4; y < y0 + 20; ++y) {
        for (int x = x0 - 4; x < x0 + 20; ++x) {
            lv_color_t color = frame[y * WIDTH + x];
            int kind = color.full == neutral.full ? 0 :
                       color.full == green.full ? 1 :
                       color.full == orange.full ? 2 : -1;
            if (kind < 0) continue;
            assert(x >= x0 && x < x0 + 16 && y >= y0 && y < y0 + 16 &&
                   "forecast pixels must stay inside the 16x16 label gap");
            ++counts[kind];
        }
    }
    if (counts[expected] < 40u)
        fprintf(stderr, "forecast row %d expected %d, counts unknown/headroom/tight: %u/%u/%u\n",
                row, expected, counts[0], counts[1], counts[2]);
    assert(counts[expected] >= 40u);
    for (int i = 0; i < 3; ++i)
        if (i != expected) assert(counts[i] == 0 && "one forecast state per row");
    const lv_color_t expected_color = expected == DISPLAY_FORECAST_UNKNOWN ? neutral :
                                      expected == DISPLAY_FORECAST_HEADROOM ? green : orange;
    assert(frame[(y0 + 8) * WIDTH + x0 + 8].full == expected_color.full &&
           "forecast circle center must show the classified color");
    assert(frame[(y0 + 3) * WIDTH + x0 + 3].full == lv_color_hex(0x000000).full &&
           frame[(y0 + 12) * WIDTH + x0 + 12].full == lv_color_hex(0x000000).full &&
           "forecast indicators must have round corners");
    assert(frame[y0 * WIDTH + (x0 - 1)].full == lv_color_hex(0x000000).full &&
           frame[y0 * WIDTH + (x0 + 16)].full == lv_color_hex(0x000000).full &&
           "forecast symbol cannot add a card or pill background");
}

static void assert_percent_glyph(int row, int digits)
{
    int y0 = row == 0 ? 112 : 262;
    int x0 = digits == 3 ? 126 : 98;
    unsigned ink = 0;
    lv_refr_now(NULL);
    for (int y = y0; y < y0 + 40; ++y)
        for (int x = x0; x < x0 + 35; ++x) {
            lv_color32_t color;
            color.full = lv_color_to32(frame[y * WIDTH + x]);
            if (color.ch.red > 180 && color.ch.green > 180 && color.ch.blue > 180)
                ++ink;
        }
    assert(ink > 40 && "complete percent glyph must be present in captured framebuffer");
}

static void forecast_scene(const char *base)
{
    provider_usage_t usage = example_provider(10.0f, 5.0f, 18000, 604800);
    usage.windows[0].resets_at = mock_epoch + (18000 - 7200);
    usage.windows[1].resets_at = mock_epoch + (604800 - 2 * 86400);
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    open_claude_detail();
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_HEADROOM);
    assert_percent_glyph(0, 2);
    assert_percent_glyph(1, 2);
    assert_flat_dashboard_pixels(0);
    save_named_frame(base, "headroom");

    usage.windows[0].used_percent = 50.0f;
    usage.windows[1].used_percent = 30.0f;
    usage.sampled_us = fake_us;
    usage.sampled_at = mock_epoch;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_TIGHT);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    assert_percent_glyph(0, 2);
    assert_percent_glyph(1, 2);
    save_named_frame(base, "tight");
    usage.windows[0].used_percent = 10.0f;
    usage.windows[1].valid = false;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "unknown-week");
    usage.windows[0].valid = false;
    usage.windows[1].valid = true;
    usage.windows[1].used_percent = 30.0f;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    save_named_frame(base, "unknown-five");
    usage.windows[0].valid = true;
    usage.windows[0].used_percent = 10.0f;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    save_named_frame(base, "mixed");

    mock_epoch += 10;
    step_ms(10000);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    usage.windows[0].used_percent = 50.0f;
    usage.windows[1].used_percent = 5.0f;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_TIGHT);
    assert_forecast_pixels(1, DISPLAY_FORECAST_HEADROOM);

    usage.windows[0].used_percent = 0.0f;
    usage.windows[1].used_percent = 0.0f;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "zero");
    usage.windows[0].used_percent = 10.0f;
    usage.windows[1].used_percent = 10.0f;
    usage.windows[0].resets_at = mock_epoch + 60;
    usage.windows[1].resets_at = mock_epoch + 60;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_HEADROOM);
    save_named_frame(base, "early");
    usage.windows[0].resets_at = 0;
    usage.windows[1].resets_at = 0;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "missing");

    usage.windows[0].resets_at = mock_epoch + (18000 - 7200);
    usage.windows[1].resets_at = mock_epoch + (604800 - 2 * 86400);
    usage.windows[0].used_percent = 50.0f;
    usage.windows[1].used_percent = 30.0f;
    usage.sampled_at = mock_epoch;
    usage.sampled_us = fake_us;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    fake_us += 901LL * 1000000LL;
    mock_epoch += 901;
    lv_tick_inc(1000);
    lv_timer_handler();
    open_claude_detail();
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "stale");
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    display_status("Usage", "Connection failed");
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "error");
    usage.windows[0].resets_at = 0;
    usage.windows[1].resets_at = 0;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &usage);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "expired");
}

static void forecast_preclock_scene(const char *path)
{
    assert(setenv("TZ", "Europe/Berlin", 1) == 0);
    tzset();
    set_berlin_time(10, 8);
    box_settings_t settings;
    box_config_defaults(&settings);
    settings.providers = 1;
    settings.default_view = 1;
    box_config_set_current(&settings);
    display_init();
    provider_usage_t claude = example_provider(10.0f, 30.0f, 18000, 604800);
    claude.has_sample = false;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &claude);
    open_claude_detail();
    claude.has_sample = true;
    claude.sampled_at = 0; /* acquired before NTP makes wall-clock projection safe */
    claude.sampled_us = fake_us;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &claude);
    display_clock_ready();
    fake_us += 5000LL * 1000LL;
    lv_tick_inc(5000);
    lv_timer_handler();
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    claude.sampled_us = fake_us;
    claude.sampled_at = mock_epoch;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &claude);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    step_ms(35000);
    assert_clock_view();
    assert(frame[131 * WIDTH + 296].full != lv_color_hex(0x89B18B).full &&
           frame[281 * WIDTH + 295].full != lv_color_hex(0xD97757).full &&
           "clock view must hide quota forecast symbols");
    display_wifi_setup("WIFI:T:WPA;S:test;P:test;;",
                       "http://192.168.4.1/setup", false, NULL);
    lv_refr_now(NULL);
    assert(frame[131 * WIDTH + 296].full != lv_color_hex(0x89B18B).full &&
           frame[281 * WIDTH + 295].full != lv_color_hex(0xD97757).full &&
           "setup QR overlay must cover quota forecast symbols");
    assert(write_ppm(path) == 0);
}

static void assert_mascot_only_changed(const lv_color_t *reference)
{
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            if ((x >= 56 && x <= 128 && y >= 0 && y <= 56) || y >= 405) continue;
            assert(frame[y * WIDTH + x].full == reference[y * WIDTH + x].full &&
                   "mascot movement must not move or repaint other dashboard content");
        }
    }
}

static void assert_mascot_region_equal(const lv_color_t *reference)
{
    for (int y = 0; y <= 56; ++y)
        for (int x = 56; x <= 128; ++x)
            assert(frame[y * WIDTH + x].full == reference[y * WIDTH + x].full &&
                   "mascot pixels must remain still");
}

static void mascot_orange_bounds(int *min_x, int *min_y, int *max_x, int *max_y)
{
    *min_x = WIDTH;
    *min_y = HEIGHT;
    *max_x = -1;
    *max_y = -1;
    for (int y = 0; y <= 56; ++y) {
        for (int x = 56; x <= 128; ++x) {
            if (!pixel_is_orange(x, y)) continue;
            if (x < *min_x) *min_x = x;
            if (x > *max_x) *max_x = x;
            if (y < *min_y) *min_y = y;
            if (y > *max_y) *max_y = y;
        }
    }
    assert(*max_x >= *min_x && *max_y >= *min_y && "mascot orange pixels must remain visible");
}

static void assert_mascot_offset_is_bounded(void)
{
    int min_x, min_y, max_x, max_y;
    mascot_orange_bounds(&min_x, &min_y, &max_x, &max_y);
    assert(min_x >= 64 && max_x <= 120 && min_y >= 0 && max_y <= 48 &&
           "all mascot pixels must remain in the clear header area");
}

static void mascot_motion_scene(const char *base)
{
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    open_claude_detail();
    scenario_elapsed_ms = 0;
    lv_refr_now(NULL);
    lv_color_t initial[WIDTH * HEIGHT];
    memcpy(initial, frame, sizeof(initial));
    step_until(1000);
    lv_refr_now(NULL);
    assert_black_corners();
    assert_mascot_region_equal(initial);
    lv_color_t rest[WIDTH * HEIGHT];
    memcpy(rest, frame, sizeof(rest));
    save_named_frame(base, "rest");
    int rest_min_x, rest_min_y, rest_max_x, rest_max_y;
    mascot_orange_bounds(&rest_min_x, &rest_min_y, &rest_max_x, &rest_max_y);
    assert(rest_min_x == 64 && rest_min_y == 4 && rest_max_x == 107 && rest_max_y == 36 &&
           "resting mascot must preserve its original pixel geometry");
    assert(frame[18 * WIDTH + 80].full == lv_color_hex(0x000000).full &&
           frame[20 * WIDTH + 81].full == lv_color_hex(0x000000).full &&
           frame[18 * WIDTH + 92].full == lv_color_hex(0x000000).full &&
           frame[20 * WIDTH + 93].full == lv_color_hex(0x000000).full &&
           "mascot eyes must keep their original two 4x5 cutouts");

    /* Capture distinct poses at 20 ms steps without depending on exact timer phase. */
    lv_color_t previous[WIDTH * HEIGHT];
    memcpy(previous, rest, sizeof(previous));
    int captured = 0, moving_samples = 0;
    int furthest_x = rest_min_x, highest_y = rest_min_y, lowest_y = rest_min_y;
    bool left_step_seen = false, right_step_seen = false, arm_wave_seen = false;
    bool left_saved = false, right_saved = false, high_saved = false;
    bool low_saved = false, step_saved = false, bob_saved = false;
    for (uint32_t t = 1020; t <= 3800; t += 20) {
        step_until(t);
        lv_refr_now(NULL);
        int changed = 0;
        for (int y = 0; y <= 56; ++y)
            for (int x = 56; x <= 128; ++x)
                changed += frame[y * WIDTH + x].full != rest[y * WIDTH + x].full;
        if (changed == 0) continue;
        ++moving_samples;
        assert_mascot_only_changed(rest);
        assert_mascot_offset_is_bounded();
        int min_x, min_y, max_x, max_y;
        mascot_orange_bounds(&min_x, &min_y, &max_x, &max_y);
        if (min_x > furthest_x) furthest_x = min_x;
        if (min_y < highest_y) highest_y = min_y;
        if (min_y > lowest_y) lowest_y = min_y;
        int group_x = min_x - 4, group_y = min_y - 4;
        if (pixel_is_orange(group_x + 14, group_y + 29)) left_step_seen = true;
        if (pixel_is_orange(group_x + 32, group_y + 29)) right_step_seen = true;
        if (pixel_is_orange(group_x + 4, group_y + 12)) arm_wave_seen = true;
        if (min_x == rest_min_x && !left_saved) {
            save_named_frame(base, "left");
            left_saved = true;
        }
        if (min_x >= rest_min_x + 12 && !right_saved) {
            save_named_frame(base, "right");
            right_saved = true;
        }
        if (min_y <= rest_min_y - 4 && !high_saved) {
            save_named_frame(base, "high");
            high_saved = true;
        }
        if (min_y >= rest_min_y + 4 && !low_saved) {
            save_named_frame(base, "low");
            low_saved = true;
        }
        if ((left_step_seen || right_step_seen) && !step_saved) {
            save_named_frame(base, "step");
            step_saved = true;
        }
        if (min_y > rest_min_y && !bob_saved) {
            save_named_frame(base, "bob");
            bob_saved = true;
        }
        int pose_changed = 0;
        for (int y = 0; y <= 56; ++y)
            for (int x = 56; x <= 128; ++x)
                pose_changed |= frame[y * WIDTH + x].full != previous[y * WIDTH + x].full;
        if (pose_changed) {
            char name[32];
            snprintf(name, sizeof(name), "pose-%02d", ++captured);
            save_named_frame(base, name);
            memcpy(previous, frame, sizeof(previous));
        }
    }
    assert(moving_samples > 0 && captured > 0 && "idle mascot must visibly move");
    assert(furthest_x - rest_min_x >= 8 && lowest_y - highest_y >= 4 &&
           "mascot must travel visibly farther than the old shuffle");
    assert(left_step_seen && right_step_seen && arm_wave_seen &&
           "mascot must animate both feet and an arm independently");
    assert(left_saved && right_saved && high_saved && low_saved && step_saved && bob_saved &&
           "reference captures must include the full walking range");
    step_until(4000);
    lv_refr_now(NULL);
    assert_mascot_region_equal(rest);
    save_named_frame(base, "return");

    step_until(8000);
    lv_refr_now(NULL);
    assert_mascot_region_equal(rest);
    save_named_frame(base, "return-two");
}

static void mascot_pause_scene(const char *scene, const char *base)
{
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    open_claude_detail();
    scenario_elapsed_ms = 0;
    lv_refr_now(NULL);
    lv_color_t initial[WIDTH * HEIGHT];
    memcpy(initial, frame, sizeof(initial));
    bool moving = false;
    for (uint32_t t = 1020; t <= 2200 && !moving; t += 20) {
        step_until(t);
        lv_refr_now(NULL);
        for (int y = 0; y <= 56 && !moving; ++y)
            for (int x = 56; x <= 128; ++x)
                if (frame[y * WIDTH + x].full != initial[y * WIDTH + x].full) {
                    moving = true;
                    break;
                }
    }
    assert(moving && "pause cases must enter while the mascot is moving");
    lv_color_t rest[WIDTH * HEIGHT];
    memcpy(rest, initial, sizeof(rest));
    assert_mascot_offset_is_bounded();
    if (strcmp(scene, "setup") == 0) {
        display_setup("http://192.168.4.1/setup");
    } else if (strcmp(scene, "error") == 0) {
        display_status("Usage", "Connection failed; try again later");
    } else if (strcmp(scene, "offline") == 0) {
        display_status("Wi-Fi", "Connection failed; retrying");
    } else if (strcmp(scene, "updating") == 0) {
        display_updating();
    } else if (strcmp(scene, "fullface") == 0) {
        display_refresh_cycle_begin();
    } else if (strcmp(scene, "tap") == 0) {
        display_set_refresh_callback(mock_refresh, &refresh_requests);
        pointer_tap(380, 20);
        assert(refresh_requests == 1 && "manual header Update tap must still request a refresh");
    } else if (strcmp(scene, "stale") == 0) {
        fake_us += 901LL * 1000000LL;
        for (int i = 0; i < 8; ++i) {
            step_ms(20);
            scenario_elapsed_ms += 20;
        }
    } else {
        assert(!"unknown mascot pause scene");
    }
    lv_refr_now(NULL);
    lv_color_t before[WIDTH * HEIGHT];
    memcpy(before, frame, sizeof(before));
    if (strcmp(scene, "fullface") != 0) {
        for (int y = 0; y <= 56; ++y)
            for (int x = 56; x <= 128; ++x)
                assert(frame[y * WIDTH + x].full == rest[y * WIDTH + x].full &&
                       "inhibition must reset the mascot synchronously");
    }
    save_named_frame(base, "paused-start");
    uint32_t pause_duration = strcmp(scene, "fullface") == 0 ? 4000 : 8000;
    for (uint32_t elapsed = 0; elapsed < pause_duration; elapsed += 20) step_ms(20);
    lv_refr_now(NULL);
    if (strcmp(scene, "fullface") == 0) {
        /* The full-face overlay is intentionally animated; the mascot remains hidden. */
        assert(pixel_is_orange(10, 10) && "full-face scene must remain visibly active");
    } else {
            for (int y = 0; y <= 56; ++y)
                for (int x = 56; x <= 128; ++x)
                assert(frame[y * WIDTH + x].full == before[y * WIDTH + x].full &&
                       "mascot must remain paused for non-healthy display states");
    }
    save_named_frame(base, "paused-end");
}

static void assert_usage_footer(void)
{
    lv_obj_t *footer = find_label_prefix(lv_scr_act(), "Error:");
    assert(footer && "failure detail must stay visible in the footer");
    assert(lv_obj_get_style_text_font(footer, LV_PART_MAIN) == &usage_status_font &&
           "refresh footer must use the compact production status font");
    assert(lv_obj_get_x(footer) == 24 && lv_obj_get_width(footer) == 432);
    assert(lv_obj_get_y(footer) == 405 && lv_obj_get_height(footer) == 34 &&
           "long error must wrap above the timestamp");
    assert_age_label("Just now");
}

static void assert_age_label(const char *text)
{
    lv_obj_t *age = find_label(lv_scr_act(), text);
    assert(age && "last successful response age must stay visible");
    assert(!lv_obj_has_flag(age, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_text_font(age, LV_PART_MAIN) == &usage_meta_font &&
           "timestamp uses the 12 px Lato Bold font");
    assert(lv_obj_get_x(age) == 240 && lv_obj_get_y(age) == 442 &&
           lv_obj_get_width(age) == 216 && lv_obj_get_height(age) == 14 &&
           "timestamp must end 24 px from the right and bottom edges");
    assert(lv_obj_get_style_text_align(age, LV_PART_MAIN) == LV_TEXT_ALIGN_RIGHT);
}

static void assert_quiet_footer(void)
{
    lv_refr_now(NULL);
    assert(!find_label_prefix(lv_scr_act(), "* ") &&
           "decorative thinking words must not return during idle");
    for (int y = 430; y < 464; ++y)
        for (int x = 128; x < 352; ++x)
            assert(frame[y * WIDTH + x].full == lv_color_hex(0x000000).full &&
                   "idle footer clears old text pixels while preserving the age at right");
    assert_age_label("Just now");
}

static void footer_clean_scene(const char *base)
{
    box_settings_t settings = *box_config_current();
    settings.refresh_animation = false;
    box_config_set_current(&settings);
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    open_claude_detail();
    assert_quiet_footer();
    save_named_frame(base, "idle");
    step_periodic_ms(3200);
    assert_quiet_footer();
    save_named_frame(base, "after-wait");

    display_app_refresh_begin(APP_ID_CLAUDE, 701);
    lv_obj_t *updating = find_label(lv_scr_act(), "Updating...");
    assert(updating && !lv_obj_has_flag(updating, LV_OBJ_FLAG_HIDDEN) &&
           "real refresh status remains visible");
    lv_refr_now(NULL);
    save_named_frame(base, "updating");
    step_ms(20);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    display_app_refresh_end(APP_ID_CLAUDE, 701);
    assert_quiet_footer();
    save_named_frame(base, "after-refresh");

    display_status("Usage", "Connection failed");
    lv_obj_t *error = find_label_prefix(lv_scr_act(), "Error:");
    assert(error && !lv_obj_has_flag(error, LV_OBJ_FLAG_HIDDEN) &&
           "removing decorative copy must not hide failures");
    assert_age_label("Just now");
    lv_refr_now(NULL);
    save_named_frame(base, "error");
    step_ms(20);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    assert_quiet_footer();
    save_named_frame(base, "recovered");
    assert(write_ppm(base) == 0);
}

static void footer_minute_scene(const char *base)
{
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    open_claude_detail();
    display_set_refresh_callback(mock_refresh, &refresh_requests);
    assert_age_label("Just now");
    step_ms(60000);
    assert_clock_view();
    pointer_tap(10, 10);
    assert(refresh_requests == 0);
    assert_age_label("1 minute ago");
    step_ms(60000);
    assert_clock_view();
    pointer_tap(10, 10);
    assert(refresh_requests == 0);
    assert_age_label("2 minutes ago");
    assert(find_label_prefix(lv_scr_act(), "* "));
    save_ppm(base);
}

static void boot_capture(const char *base)
{
    static const char *five_reset = "2026-10-26T23:00:00Z";
    static const char *week_reset = "2026-11-02T20:00:00Z";
    display_init();
    scenario_elapsed_ms = 0;
    lv_refr_now(NULL);
    assert_face_pixel(10, 10, 0xD97757);
    assert_face_pixel(479, 479, 0xD97757);
    assert_face_eyes_open();
    assert_boot_label("Connecting to Wi-Fi...");
    assert_no_visible_digital_clock();
    save_named_frame(base, "open");

    display_status("Starting", "Connecting to Claude...");
    assert_boot_label("Connecting to Wi-Fi...");
    step_until(240);
    display_status("Wi-Fi", "Reconnecting");
    assert_boot_label("Connecting to Wi-Fi...");
    step_until(500);
    display_status("Wi-Fi", "Waiting for connection");
    assert_boot_label("Connecting to Wi-Fi...");
    step_until(760);
    display_status("Time", "Waiting for clock");
    assert_boot_label("Waiting for time...");
    display_clock_ready();
    step_until(1000);
    display_status("OAuth", "Refreshing token");
    assert_boot_label("Loading usage...");
    step_until(1200);
    display_updating();
    assert_boot_label("Loading usage...");

    /* These closed/wink poses remain anchored to display_init despite status churn. */
    step_until(1600);
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_face_pixel(160, 190, 0xD97757);
    assert_face_pixel(320, 190, 0xD97757);
    step_until(3000);
    lv_refr_now(NULL);
    assert_face_pixel(160, 190, 0xD97757);
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 190, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_boot_label("Loading usage...");
    save_named_frame(base, "status");

    /* A quick success before five seconds keeps the face up to its minimum. */
    step_until(4200);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    step_until(4640);
    assert_face_pixel(10, 10, 0xD97757);
    step_until(4800);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    step_until(5000);
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "boot face ends after its five-second minimum");
    assert_flat_dashboard_pixels(1);
    save_named_frame(base, "done");
}

static void boot_slow_scene(void)
{
    display_init();
    display_clock_ready();
    scenario_elapsed_ms = 0;
    display_status("Wi-Fi", "Waiting for connection");
    step_until(1200);
    display_status("Time", "Waiting for clock");
    display_updating();
    step_until(6000);
    lv_refr_now(NULL);
    assert(pixel_is_orange(10, 10) && "slow startup holds boot face open past five seconds");
    assert_face_eyes_open();
    assert_boot_label("Loading usage...");
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    step_until(6200);
    assert(frame[10 * WIDTH + 10].full != lv_color_hex(0x000000).full &&
           "slow success uses a short fade after data arrives");
    step_until(6380);
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "slow success completes the boot handoff");
    assert_flat_dashboard_pixels(1);
}

static void boot_interrupt_scene(void)
{
    display_init();
    display_clock_ready();
    scenario_elapsed_ms = 0;
    display_status("Wi-Fi", "Connection failed; retrying");
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "real startup failure interrupts boot face");
    assert(contains_label(lv_scr_act(), "Wi-Fi"));

    /* Even later data after setup cannot replay the one-shot boot animation. */
    display_setup("http://192.168.4.1/setup");
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10));
    assert(contains_label(lv_scr_act(), "Connect Claude"));
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    step_ms(20);
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "usage after setup cannot restart boot face");

    display_status("Usage", "Connection failed; check Wi-Fi settings and try again later");
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) &&
           "a post-setup error cannot restart the boot face");
    step_until(5000);
    assert(!pixel_is_orange(10, 10) && "post-setup status changes cannot replay the boot face");
}

static void settle_boot(void)
{
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    fake_us += 5000LL * 1000LL;
    lv_tick_inc(5000);
    lv_timer_handler();
    lv_refr_now(NULL);
}

static provider_usage_t example_provider(float first, float second,
                                         uint32_t first_window, uint32_t second_window)
{
    provider_usage_t usage = {0};
    usage.configured = true;
    usage.has_sample = true;
    usage.sampled_at = mock_epoch;
    usage.sampled_us = fake_us;
    usage.windows[0] = (quota_window_t){.valid = true, .used_percent = first,
        .resets_at = mock_epoch + 3600, .window_seconds = first_window};
    usage.windows[1] = (quota_window_t){.valid = true, .used_percent = second,
        .resets_at = mock_epoch + 86400, .window_seconds = second_window};
    return usage;
}

static void assert_provider_chrome(void)
{
    unsigned heading = 0, mascot = 0, settings = 0;
    lv_color_t light = lv_color_hex(0xF9F6EF);
    lv_color_t clay = lv_color_hex(0xD97757);
    for (int y = 24; y < 72; ++y) {
        for (int x = 140; x < 340; ++x)
            heading += frame[y * WIDTH + x].full == light.full;
        for (int x = 24; x < 105; ++x)
            mascot += frame[y * WIDTH + x].full == clay.full;
    }
    for (int y = 440; y < 457; ++y)
        for (int x = 24; x < 108; ++x)
            settings += frame[y * WIDTH + x].full != lv_color_hex(0x000000).full;
    assert(heading > 40 && mascot > 10 && settings > 10 &&
           "provider dashboard chrome must paint after first boot face");
}

static void provider_scene(const char *scene, const char *output)
{
    box_settings_t settings;
    box_config_defaults(&settings);
    settings.providers = (strcmp(scene, "provider-codex") == 0 ||
                          strcmp(scene, "provider-codex-zero") == 0 ||
                          strcmp(scene, "provider-weekly-only") == 0 ||
                          strcmp(scene, "provider-unconfigured") == 0) ? 1 : 2;
    settings.default_view = 1;
    settings.refresh_animation = false;
    box_config_set_current(&settings);
    display_init();
    display_clock_ready();
    provider_usage_t claude = example_provider(50.0f, 20.0f, 18000, 604800);
    provider_usage_t codex = example_provider(30.0f, 80.0f, 14400, 86400);
    codex.reset_credits_known = true;
    codex.reset_credits = strcmp(scene, "provider-codex-zero") == 0 ? 0 : 3;
    snprintf(codex.status, sizeof(codex.status), "Ready");
    if (settings.providers == 2) display_provider_update(USAGE_PROVIDER_CLAUDE, &claude);
    if (strcmp(scene, "provider-unconfigured") == 0) {
        codex.configured = false;
        codex.has_sample = false;
    } else if (strcmp(scene, "provider-missing") == 0) codex.has_sample = false;
    else if (strcmp(scene, "provider-unknown") == 0) codex.windows[1].valid = false;
    else if (strcmp(scene, "provider-weekly-only") == 0) codex.windows[0].valid = false;
    display_provider_update(USAGE_PROVIDER_CODEX, &codex);
    for (int i = 0; i < 260; ++i) step_ms(20);
    lv_refr_now(NULL);
    assert_black_corners();
    assert_provider_chrome();
    assert(contains_label(lv_scr_act(), "Codex"));
    if (strcmp(scene, "provider-codex-zero") == 0)
        assert(contains_label(lv_scr_act(), "Reset tokens: 0"));
    else if (strcmp(scene, "provider-missing") != 0 &&
        strcmp(scene, "provider-unconfigured") != 0)
        assert(contains_label(lv_scr_act(), "Reset tokens: 3"));
    if (settings.providers == 2) {
        assert(contains_label(lv_scr_act(), "Claude"));
        assert(frame[198 * WIDTH + 80].full ==
               lv_color_hex(display_quota_color(50.0f)).full);
        assert(frame[348 * WIDTH + 80].full ==
               lv_color_hex(display_quota_color(70.0f)).full ||
               strcmp(scene, "provider-missing") == 0);
        if (strcmp(scene, "provider-missing") != 0)
            assert(contains_label(lv_scr_act(), "4h left"));
        if (strcmp(scene, "provider-missing") == 0)
            assert(contains_label(lv_scr_act(), "Waiting | Reset tokens: 3"));
        if (strcmp(scene, "provider-unknown") == 0)
        {
            assert(contains_label(lv_scr_act(), "--%"));
            assert(frame[348 * WIDTH + 80].full ==
                   lv_color_hex(display_quota_color(70.0f)).full);
            assert(frame[348 * WIDTH + 260].full == lv_color_hex(0x000000).full &&
                   "missing Codex window cannot invent a bar");
        }
    } else {
        if (strcmp(scene, "provider-unconfigured") == 0) {
            assert(contains_label(lv_scr_act(), "Set up Codex in web app"));
            display_set_settings_callback(mock_settings, &settings_opens);
            assert(write_ppm(output) == 0);
            pointer_tap(48, 442);
            assert(settings_opens == 1 && "unconfigured Codex must allow Settings");
            return;
        }
        if (strcmp(scene, "provider-weekly-only") != 0) {
            assert(contains_label(lv_scr_act(), "4h left"));
            assert(contains_label(lv_scr_act(), "70%"));
        }
    }
    if (strcmp(scene, "provider-weekly-only") == 0) {
        assert(contains_label(lv_scr_act(), "--%"));
        assert(frame[340 * WIDTH + 80].full ==
               lv_color_hex(display_quota_color(20.0f)).full);
        assert(write_ppm(output) == 0);
        pointer_tap(405, 40);
        assert(!lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN) &&
               "weekly-only Codex usage must allow healthy clock view");
        return;
    }
    if (strcmp(scene, "provider-stale") == 0) {
        fake_us += 901LL * 1000000LL;
        codex.sampled_us = fake_us;
        display_provider_update(USAGE_PROVIDER_CODEX, &codex);
        step_ms(20);
        assert(contains_label(lv_scr_act(), "Stale"));
        assert_age_label("15 minutes ago");
        assert(frame[198 * WIDTH + 80].full !=
               lv_color_hex(display_quota_color(50.0f)).full);
        assert(frame[348 * WIDTH + 80].full ==
               lv_color_hex(display_quota_color(70.0f)).full);
        assert(write_ppm(output) == 0);
        pointer_tap(405, 40);
        assert(lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN) &&
               "fresh Codex must not hide stale Claude via clock view");
        return;
    }
    if (strcmp(scene, "provider-attention") == 0) {
        codex.updating = true;
        snprintf(codex.status, sizeof(codex.status), "Checking usage");
        display_provider_update(USAGE_PROVIDER_CODEX, &codex);
        for (int i = 0; i < 31; ++i) step_ms(1000);
        assert(lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN) &&
               "provider-specific update keeps Both dashboard visible");
        assert(contains_label(lv_scr_act(), "Codex: Checking usage"));
        pointer_tap(405, 40);
        assert(!lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN) &&
               "manual clock override remains available during a fresh poll");
        codex.updating = false;
        snprintf(codex.status, sizeof(codex.status), "Usage unavailable; retrying");
        display_provider_update(USAGE_PROVIDER_CODEX, &codex);
        for (int i = 0; i < 31; ++i) step_ms(1000);
        assert(lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN) &&
               "provider error returns attention from manual clock");
        assert(contains_label(lv_scr_act(), "Codex: Usage unavailable; retrying"));
        assert_provider_chrome();
        assert(frame[198 * WIDTH + 80].full ==
               lv_color_hex(display_quota_color(50.0f)).full);
        assert(write_ppm(output) == 0);
        return;
    }
    assert(write_ppm(output) == 0);
}

static void prepare_framework_display(uint8_t providers, uint8_t default_view,
                                      bool refresh_animation)
{
    assert(setenv("TZ", "Europe/Berlin", 1) == 0);
    tzset();
    set_berlin_time(10, 8);
    box_settings_t settings;
    box_config_defaults(&settings);
    settings.providers = providers;
    settings.default_view = default_view;
    settings.refresh_animation = refresh_animation;
    settings.mascot_motion = false;
    box_config_set_current(&settings);
    display_init();
    display_set_device_name("hello-box-4827");
    display_clock_ready();
    if (providers == 0 || providers == 2) {
        provider_usage_t claude = example_provider(50.0f, 20.0f, 18000, 604800);
        claude.windows[0].resets_at = time(NULL) + 3600;
        claude.windows[1].resets_at = time(NULL) + 86400;
        claude.reset_credits_known = true;
        claude.reset_credits = 0;
        snprintf(claude.status, sizeof(claude.status), "Ready");
        display_provider_update(USAGE_PROVIDER_CLAUDE, &claude);
    }
    if (providers == 1 || providers == 2) {
        /* Codex exposes the weekly window only; use the real 7-day contract. */
        provider_usage_t codex = example_provider(30.0f, 20.0f, 14400, 604800);
        codex.windows[1].resets_at = time(NULL) + 86400;
        codex.reset_credits_known = true;
        codex.reset_credits = 3;
        snprintf(codex.status, sizeof(codex.status), "Ready");
        display_provider_update(USAGE_PROVIDER_CODEX, &codex);
    }
    step_periodic_ms(5000);
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "boot face must finish before shell route tests");
    lv_obj_t *home = find_screen_box(0, 56, 480, 424);
    assert(home && !lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN));
}

static void shell_ui_scene(const char *base)
{
    prepare_framework_display(2, 1, false);
    assert(contains_label(lv_scr_act(), "Home"));
    assert(contains_label(lv_scr_act(), "50%") && contains_label(lv_scr_act(), "80%"));
    assert(contains_label(lv_scr_act(), "Reset tokens: 0") &&
           contains_label(lv_scr_act(), "Reset tokens: 3"));
    lv_obj_t *home = find_screen_box(0, 56, 480, 424);
    lv_obj_t *claude_slot = find_descendant_box(home, 8, 64, 464, 200);
    lv_obj_t *codex_slot = find_descendant_box(home, 8, 272, 464, 200);
    lv_obj_t *claude_value = find_label(claude_slot, "50%");
    lv_obj_t *codex_value = find_label(codex_slot, "80%");
    lv_obj_t *claude_period = find_label(claude_slot, "5h left");
    lv_obj_t *codex_period = find_label(codex_slot, "Week left");
    lv_obj_t *claude_count = find_label_prefix(claude_slot, "Resets in");
    lv_obj_t *codex_count = find_label_prefix(codex_slot, "Resets in");
    assert(claude_value && codex_value && claude_period && codex_period &&
           claude_count && codex_count &&
           !lv_obj_has_flag(claude_value, LV_OBJ_FLAG_HIDDEN) &&
           !lv_obj_has_flag(codex_value, LV_OBJ_FLAG_HIDDEN) &&
           !lv_obj_has_flag(find_label(claude_slot, "Reset tokens: 0"), LV_OBJ_FLAG_HIDDEN) &&
           !lv_obj_has_flag(find_label(codex_slot, "Reset tokens: 3"), LV_OBJ_FLAG_HIDDEN) &&
           "configured Home shows both providers and known reset-token counts");
    assert(lv_obj_get_style_text_font(claude_value, LV_PART_MAIN) == &usage_values_font &&
           lv_obj_get_style_text_font(codex_value, LV_PART_MAIN) == &usage_values_font &&
           lv_obj_get_style_text_font(claude_period, LV_PART_MAIN) == &usage_status_font &&
           lv_obj_get_style_text_font(codex_count, LV_PART_MAIN) == &usage_status_font &&
           "provider values are 48px and countdowns use readable 16px text");
    assert_header_targets(true);
    assert_shell_header_ink(true);
    assert_home_widget_pixels();
    save_named_frame(base, "home-both");
    pointer_tap(316, 24); /* Former clock location is empty on Home. */
    assert_home_widget_pixels();
    assert_no_visible_digital_clock();

    /* Real LVGL pointer input opens each configured app detail from its tile. */
    pointer_tap(240, 160);
    assert(contains_label(lv_scr_act(), "Claude"));
    assert_header_targets(true);
    assert_shell_header_ink(false);
    assert_app_detail_pixels(0);
    save_named_frame(base, "claude-app");
    pointer_tap(32, 24);
    assert_home_widget_pixels();
    pointer_tap(240, 360);
    assert(contains_label(lv_scr_act(), "Codex"));
    assert(contains_label(lv_scr_act(), "Reset tokens: 3"));
    assert_header_targets(true);
    assert_shell_header_ink(false);
    save_named_frame(base, "codex-app");
    pointer_tap(316, 24); /* The former clock hotspot is inert on app detail too. */
    assert(contains_label(lv_scr_act(), "Codex"));
    assert_no_visible_digital_clock();

    /* Home returns from app detail; no header clock shortcut exists. */
    pointer_tap(32, 24);
    assert_home_widget_pixels();
    assert_no_visible_digital_clock();
    save_named_frame(base, "home-no-clock-shortcut");

    /* Home refresh targets ALL and tracks its two accepted providers separately. */
    memset(&app_refresh_log, 0, sizeof(app_refresh_log));
    app_refresh_log.accepted_mask = (1u << APP_ID_CLAUDE) | (1u << APP_ID_CODEX);
    display_set_app_refresh_callback(mock_app_refresh, &app_refresh_log);
    pointer_tap(356, 0); /* Top-left edge of the Update target. */
    assert(app_refresh_log.calls == 1 && app_refresh_log.targets[0] == APP_ID_ALL &&
           "Home Update requests all enabled apps");
    pointer_tap(403, 55);
    assert(app_refresh_log.calls == 1 && "accepted Home refresh blocks repeated taps");
    display_app_refresh_begin(APP_ID_CLAUDE, 1);
    pointer_tap(380, 20);
    assert(app_refresh_log.calls == 1 && "Codex's pending refresh keeps Home Update gated");
    display_updating();
    display_app_refresh_end(APP_ID_CLAUDE, 1);
    assert_home_widget_pixels();
    pointer_tap(380, 20);
    assert(app_refresh_log.calls == 1 &&
           "the delayed Codex bit stays gated after Claude ends");
    display_app_refresh_begin(APP_ID_CODEX, 1);
    pointer_tap(380, 20);
    assert(app_refresh_log.calls == 1 && "active Codex cycle blocks Home Update");
    display_app_refresh_end(APP_ID_CODEX, 1);
    assert_home_widget_pixels();

    /* A false admission is retryable; app detail sends only its own ID. */
    app_refresh_log.accepted_mask = 0;
    pointer_tap(240, 160);
    assert(contains_label(lv_scr_act(), "Claude"));
    pointer_tap(356, 0);
    assert(app_refresh_log.calls == 2 && app_refresh_log.targets[1] == APP_ID_CLAUDE);
    app_refresh_log.accepted_mask = 1u << APP_ID_CLAUDE;
    pointer_tap(356, 0);
    assert(app_refresh_log.calls == 3 && app_refresh_log.targets[2] == APP_ID_CLAUDE &&
           "Claude detail refresh must not request Codex");
    display_app_refresh_begin(APP_ID_CLAUDE, 2);
    display_updating();
    app_refresh_log.accepted_mask = 1u << APP_ID_CODEX;
    pointer_tap(380, 20);
    assert(app_refresh_log.calls == 3 && "active Claude cycle gates duplicate detail refresh");
    display_app_refresh_end(APP_ID_CLAUDE, 2);
    pointer_tap(32, 24);
    assert_home_widget_pixels();
    save_named_frame(base, "home-refresh-complete");

    /* The former clock hotspot does not escape a queued Home ALL refresh. Each
     * accepted provider stays independently gated until its own worker begins. */
    app_refresh_log.accepted_mask = (1u << APP_ID_CLAUDE) | (1u << APP_ID_CODEX);
    pointer_tap(356, 0);
    assert(app_refresh_log.calls == 4 && app_refresh_log.targets[3] == APP_ID_ALL);
    pointer_tap(300, 24);
    assert_home_widget_pixels();
    assert_no_visible_digital_clock();
    save_named_frame(base, "home-pending-all");
    display_app_refresh_begin(APP_ID_CLAUDE, 3);
    display_updating();
    assert_home_widget_pixels();
    display_app_refresh_end(APP_ID_CLAUDE, 3);
    assert_home_widget_pixels(); /* Claude completion cannot clear Codex's pending bit. */
    save_named_frame(base, "home-after-claude");
    display_app_refresh_begin(APP_ID_CODEX, 3);
    display_updating();
    assert_home_widget_pixels();
    display_app_refresh_end(APP_ID_CODEX, 3);
    provider_usage_t delayed_codex = example_provider(25.0f, 15.0f, 14400, 86400);
    delayed_codex.sampled_us = fake_us + 1;
    fake_us += 1;
    display_provider_update(USAGE_PROVIDER_CODEX, &delayed_codex);
    assert_home_widget_pixels();
    assert_no_visible_digital_clock();
    save_named_frame(base, "home-after-codex");

    /* Header mascot invokes the existing five-second face without HTTP. */
    step_ms(5000);
    pointer_tap(88, 24);
    step_ms(320);
    assert(pixel_is_orange(10, 10) && "Clawd tap should show the large thinking face");
    assert(app_refresh_log.calls == 4 && "Clawd animation is not a network request");
    save_named_frame(base, "mascot-face");
    step_ms(4680);
    assert_home_widget_pixels();
    assert(write_ppm(base) == 0);
}

static void shell_attention_scene(const char *base)
{
    prepare_framework_display(2, 0, false);
    assert_board_view();
    assert_no_visible_digital_clock();
    pointer_tap(316, 24); /* The former clock hotspot cannot reveal the saver early. */
    assert_board_view();
    assert_no_visible_digital_clock();
    step_periodic_ms(31000); /* Let the configured idle timeout and refresh tick elapse. */
    assert_clock_view();
    save_named_frame(base, "saver");

    provider_usage_t claude = example_provider(45.0f, 18.0f, 18000, 604800);
    claude.reset_credits_known = true;
    claude.reset_credits = 1;
    snprintf(claude.status, sizeof(claude.status), "Ready");
    display_provider_update(USAGE_PROVIDER_CLAUDE, &claude);
    assert_home_widget_pixels();
    save_named_frame(base, "fresh-home-attention");

    /* A status-only and duplicate cached push cannot extend the eight seconds. */
    step_periodic_ms(4000);
    provider_usage_t status_only = claude;
    status_only.sampled_us = fake_us;
    status_only.updating = true;
    snprintf(status_only.status, sizeof(status_only.status), "Checking usage");
    display_provider_update(USAGE_PROVIDER_CLAUDE, &status_only);
    assert_home_widget_pixels();
    step_periodic_ms(3999);
    assert_home_widget_pixels();
    step_periodic_ms(1);
    assert_clock_view();
    save_named_frame(base, "lease-return-clock");

    /* A Codex payload with only short windows must not wake the saver. The
     * provider's attention capability is the same weekly window used by its
     * Home and detail widgets, not any arbitrary usage sample. */
    box_settings_t settings = *box_config_current();
    settings.providers = 1; /* Codex only. */
    box_config_set_current(&settings);
    provider_usage_t codex = example_provider(30.0f, 20.0f, 14400, 86400);
    codex.sampled_us = fake_us + 1000;
    fake_us += 1000;
    display_provider_update(USAGE_PROVIDER_CODEX, &codex);
    assert_clock_view();
    save_named_frame(base, "codex-short-only-does-not-interrupt");

    codex.windows[0] = (quota_window_t){0};
    codex.windows[1] = (quota_window_t){.valid = true, .used_percent = 35.0f,
        .resets_at = mock_epoch + 5 * 86400, .window_seconds = 604800};
    codex.sampled_us = fake_us + 1000;
    fake_us += 1000;
    display_provider_update(USAGE_PROVIDER_CODEX, &codex);
    assert(contains_label(lv_scr_act(), "Codex"));
    lv_refr_now(NULL);
    assert(frame[284 * WIDTH + 60].full ==
               lv_color_hex(display_quota_color(65.0f)).full &&
           frame[284 * WIDTH + 420].full == lv_color_hex(0x000000).full &&
           "Codex attention shows only the actual weekly quota and black unused space");
    save_named_frame(base, "codex-weekly-attention");
    step_periodic_ms(8000);
    assert_clock_view();

    /* Provider detail remains an attention target when exactly one is enabled. */
    settings = *box_config_current();
    settings.providers = 0;
    box_config_set_current(&settings);
    step_periodic_ms(30000);
    assert_clock_view();
    claude.sampled_us = fake_us + 1000;
    fake_us += 1000;
    display_provider_update(USAGE_PROVIDER_CLAUDE, &claude);
    assert(contains_label(lv_scr_act(), "Claude"));
    assert_app_detail_pixels(0);
    save_named_frame(base, "single-app-attention");
    assert(write_ppm(base) == 0);
}

static void shell_guidance_scene(const char *base)
{
    prepare_framework_display(2, 1, false);
    memset(&settings_opens, 0, sizeof(settings_opens));
    display_set_settings_callback(mock_settings, &settings_opens);

    /* A remembered but now unconfigured Codex snapshot must not leak in Home. */
    provider_usage_t codex = example_provider(30.0f, 20.0f, 14400, 86400);
    codex.configured = false;
    codex.has_sample = true;
    codex.reset_credits_known = true;
    codex.reset_credits = 12;
    snprintf(codex.status, sizeof(codex.status), "Ready");
    display_provider_update(USAGE_PROVIDER_CODEX, &codex);
    lv_obj_t *home = find_screen_box(0, 56, 480, 424);
    lv_obj_t *codex_slot = find_descendant_box(home, 8, 272, 464, 200);
    lv_obj_t *cached_percent = find_label(codex_slot, "80%");
    assert(cached_percent && lv_obj_has_flag(cached_percent, LV_OBJ_FLAG_HIDDEN) &&
           "unconfigured provider must hide every cached percentage");
    assert(find_label(home, "Reset tokens: 3") &&
           lv_obj_has_flag(find_label(home, "Reset tokens: 3"), LV_OBJ_FLAG_HIDDEN) &&
           !find_label(home, "Reset tokens: 12") &&
           "unconfigured provider must hide cached reset tokens without publishing new ones");
    assert_home_widget_pixels();
    assert(codex_slot && contains_label(codex_slot, "Set up Codex") &&
           count_pixel_color(24, 350, 132, 393, 0xF9F6EF) == 0 &&
           "unconfigured tile shows setup guidance instead of cached quota values");
    save_named_frame(base, "unconfigured-widget");

    pointer_tap(240, 360);
    assert(settings_opens == 0 &&
           "tile guidance QR must not open the Claude account server");
    lv_obj_t *overlay = overlay_for_title("Set up Codex");
    assert(contains_label(overlay, "Connect USB in Chrome or Edge"));
    assert_overlay_qr(overlay, true);
    save_named_frame(base, "codex-guidance");
    pointer_tap(60, 440);
    assert_home_widget_pixels();
    assert(settings_opens == 0 && "Back from guidance does not request account Settings");
    save_named_frame(base, "guidance-back");

    /* A disabled provider also shows setup guidance and no old quota values. */
    box_settings_t settings = *box_config_current();
    settings.providers = 0; /* Claude only; Codex remains registered but disabled. */
    box_config_set_current(&settings);
    step_ms(1000);
    lv_obj_t *disabled_codex_slot = find_descendant_box(home, 8, 272, 464, 200);
    lv_obj_t *disabled_codex_value = find_label(disabled_codex_slot, "80%");
    assert(disabled_codex_value &&
           lv_obj_has_flag(disabled_codex_value, LV_OBJ_FLAG_HIDDEN) &&
           contains_label(disabled_codex_slot, "Set up Codex") &&
           "disabling Codex hides its cached weekly value but leaves Claude visible");
    pointer_tap(240, 360);
    assert(settings_opens == 0);
    assert(overlay_for_title("Set up Codex"));
    save_named_frame(base, "codex-disabled-guidance");
    assert(write_ppm(base) == 0);
}

static void shell_codex_weekly_scene(const char *base)
{
    prepare_framework_display(2, 1, false);
    provider_usage_t codex = example_provider(30.0f, 20.0f, 14400, 604800);
    codex.windows[0] = (quota_window_t){0}; /* Codex exposes no short window. */
    codex.windows[1].window_seconds = 604800;
    codex.windows[1].resets_at = time(NULL) + 5 * 86400;
    codex.sampled_us = fake_us + 1;
    fake_us += 1;
    display_provider_update(USAGE_PROVIDER_CODEX, &codex);
    lv_refr_now(NULL);

    lv_obj_t *home = find_screen_box(0, 56, 480, 424);
    assert(home && !lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t *codex_slot = find_descendant_box(home, 8, 272, 464, 200);
    assert(codex_slot && !lv_obj_has_flag(codex_slot, LV_OBJ_FLAG_HIDDEN) &&
           "Codex gets the full-width lower Home slot");
    assert(find_label(codex_slot, "Codex") &&
           find_label(codex_slot, "Week left") &&
           find_label(codex_slot, "80%") &&
           "the Codex Home widget shows its actual weekly quota");
    assert(!contains_label(codex_slot, "Reset tokens") &&
           "unknown reset-token counts are omitted from the Home widget");
    lv_obj_t *home_hero = find_label(codex_slot, "80%");
    assert(home_hero &&
           lv_obj_get_style_text_font(home_hero, LV_PART_MAIN) == &usage_values_font &&
           "Codex Home uses the large weekly value font");
    assert(!contains_label(codex_slot, "5h left") &&
           !contains_label(codex_slot, "4h left") &&
           !contains_label(codex_slot, "24h left") &&
           !contains_label(codex_slot, "--%") &&
           "Codex must not invent a short-window placeholder or quota");
    assert(count_pixel_color(24, 393, 456, 407,
                            display_quota_color(80.0f)) >= 100 &&
           "the real weekly quota should have a rendered proportional bar");
    save_named_frame(base, "home");

    pointer_tap(240, 360);
    lv_obj_t *detail = find_screen_box(24, 90, 432, 344);
    assert(detail && !lv_obj_has_flag(detail, LV_OBJ_FLAG_HIDDEN) &&
           "tapping the Codex widget opens its app detail");
    lv_obj_t *weekly_card = find_descendant_box(detail, 24, 108, 432, 282);
    assert(weekly_card && find_label(weekly_card, "Week left") &&
           find_label(detail, "80%") &&
           lv_obj_get_style_text_font(find_label(detail, "80%"), LV_PART_MAIN) ==
               &usage_hero_font &&
           !contains_label(detail, "5h left") &&
           !contains_label(detail, "4h left") &&
           !contains_label(detail, "24h left") &&
           !contains_label(detail, "--%") &&
           !find_descendant_box(detail, 24, 240, 432, 132) &&
           "Codex detail contains one 80px hero panel for its weekly quota only");
    assert(frame[284 * WIDTH + 60].full ==
               lv_color_hex(display_quota_color(80.0f)).full &&
           frame[284 * WIDTH + 420].full == lv_color_hex(0x000000).full &&
           "Codex detail renders the actual weekly fill with black unused space");
    save_named_frame(base, "detail");

    /* Some provider payloads put the weekly limit in the primary slot. */
    pointer_tap(32, 24);
    codex.windows[0] = (quota_window_t){.valid = true, .used_percent = 20.0f,
        .resets_at = time(NULL) + 5 * 86400, .window_seconds = 604800};
    codex.windows[1] = (quota_window_t){0};
    codex.sampled_us = fake_us + 1;
    fake_us += 1;
    display_provider_update(USAGE_PROVIDER_CODEX, &codex);
    assert_home_widget_pixels();
    codex_slot = find_descendant_box(find_screen_box(0, 56, 480, 424),
                                     8, 272, 464, 200);
    assert(codex_slot && find_label(codex_slot, "Week left") &&
           find_label(codex_slot, "80%") &&
           !contains_label(codex_slot, "4h left") &&
           "weekly selection works when the provider puts it in the primary slot");
    save_named_frame(base, "home-primary-week");
    pointer_tap(240, 360);
    detail = find_screen_box(24, 90, 432, 344);
    weekly_card = find_descendant_box(detail, 24, 108, 432, 282);
    assert(weekly_card && find_label(weekly_card, "Week left") &&
           find_label(detail, "80%") &&
           lv_obj_get_style_text_font(find_label(detail, "80%"), LV_PART_MAIN) ==
               &usage_hero_font && !contains_label(detail, "4h left") &&
           "Codex detail chooses the weekly window independent of slot order");

    /* A short-only response must show weekly unavailable, never masquerade as 4h. */
    codex.windows[0] = (quota_window_t){.valid = true, .used_percent = 30.0f,
        .resets_at = time(NULL) + 3600, .window_seconds = 14400};
    codex.windows[1] = (quota_window_t){0};
    codex.sampled_us = fake_us + 1;
    fake_us += 1;
    display_provider_update(USAGE_PROVIDER_CODEX, &codex);
    lv_refr_now(NULL);
    lv_obj_t *unavailable_period = find_label(detail, "Weekly usage unavailable");
    lv_obj_t *hidden_hero = find_label(weekly_card, "80%");
    lv_obj_t *unknown_footer = find_label(detail, "Unknown");
    assert(unavailable_period && !lv_obj_has_flag(unavailable_period, LV_OBJ_FLAG_HIDDEN) &&
           hidden_hero && lv_obj_has_flag(hidden_hero, LV_OBJ_FLAG_HIDDEN) &&
           !contains_label(detail, "4h left") && !contains_label(detail, "5h left") &&
           !contains_label(detail, "24h left") &&
           !contains_label(detail, "Reset unknown") &&
           (!unknown_footer || lv_obj_has_flag(unknown_footer, LV_OBJ_FLAG_HIDDEN)) &&
           "a short-only provider payload hides the hero and states weekly data unavailable");
    assert(frame[284 * WIDTH + 60].full == lv_color_hex(0x000000).full &&
           "missing weekly data has no colored quota bar");
    save_named_frame(base, "detail-weekly-unavailable");
    assert(write_ppm(base) == 0);
}

static lv_obj_t *find_clock(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *text = lv_label_get_text(object);
        if (strlen(text) == 5 && text[2] == ':' &&
            text[0] >= '0' && text[0] <= '9' && text[1] >= '0' && text[1] <= '9' &&
            text[3] >= '0' && text[3] <= '9' && text[4] >= '0' && text[4] <= '9')
            return object;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i) {
        lv_obj_t *found = find_clock(lv_obj_get_child(object, i));
        if (found) return found;
    }
    return NULL;
}

static void assert_no_visible_digital_clock(void)
{
    lv_refr_now(NULL);
    assert(!find_clock(lv_scr_act()) &&
           "digital HH:MM must be absent outside the word-clock saver");
}

static void set_berlin_time(int hour, int minute)
{
    struct tm local;
    assert(localtime_r(&mock_epoch, &local));
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    mock_epoch = mktime(&local);
    assert(mock_epoch != (time_t)-1);
}

static void example_usage(void)
{
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
}

static lv_obj_t *word_layer(void)
{
    lv_obj_t *screen = lv_scr_act();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(screen); i++) {
        lv_obj_t *candidate = lv_obj_get_child(screen, i);
        if (lv_obj_get_child_cnt(candidate) < WORD_CLOCK_CELLS + 4) continue;
        lv_obj_t *first = lv_obj_get_child(candidate, 0);
        if (lv_obj_check_type(first, &lv_label_class) &&
            (strcmp(lv_label_get_text(first), "E") == 0 ||
             strcmp(lv_label_get_text(first), "I") == 0) &&
            lv_obj_get_x(first) == 68 && lv_obj_get_y(first) == 80)
            return candidate;
    }
    assert(!"word clock layer must exist");
    return NULL;
}

static lv_obj_t *header_mascot(void)
{
    lv_obj_t *screen = lv_scr_act();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(screen); ++i) {
        lv_obj_t *candidate = lv_obj_get_child(screen, i);
        if (lv_obj_get_width(candidate) == 68 && lv_obj_get_height(candidate) == 56 &&
            lv_obj_get_child_cnt(candidate) >= 8)
            return candidate;
    }
    assert(!"header mascot must exist");
    return NULL;
}

static lv_obj_t *find_clickable_rect(lv_obj_t *screen, int x, int y, int width, int height)
{
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(screen); ++i) {
        lv_obj_t *candidate = lv_obj_get_child(screen, i);
        if (lv_obj_get_x(candidate) == x && lv_obj_get_y(candidate) == y &&
            lv_obj_get_width(candidate) == width && lv_obj_get_height(candidate) == height &&
            lv_obj_has_flag(candidate, LV_OBJ_FLAG_CLICKABLE)) return candidate;
    }
    return NULL;
}

static void assert_header_targets(bool visible)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *home_layer = find_screen_box(0, 56, 480, 424);
    bool home_route = home_layer && !lv_obj_has_flag(home_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *home = find_clickable_rect(screen, 8, 0, 48, 56);
    lv_obj_t *mascot = home_route ? find_clickable_rect(screen, 16, 0, 80, 56)
                                  : find_clickable_rect(screen, 60, 0, 56, 56);
    lv_obj_t *refresh = find_clickable_rect(screen, 356, 0, 48, 56);
    lv_obj_t *settings = find_clickable_rect(screen, 408, 0, 48, 56);
    assert(home && mascot && refresh && settings &&
           "new header hitboxes must exist at their route-specific contract bounds");
    assert(lv_obj_get_style_bg_opa(mascot, LV_PART_MAIN) == LV_OPA_TRANSP &&
           lv_obj_get_style_bg_opa(home, LV_PART_MAIN) == LV_OPA_TRANSP &&
           lv_obj_get_style_bg_opa(refresh, LV_PART_MAIN) == LV_OPA_TRANSP &&
           lv_obj_get_style_bg_opa(settings, LV_PART_MAIN) == LV_OPA_TRANSP &&
           "header targets must not paint a background");
    assert(lv_obj_has_flag(mascot, LV_OBJ_FLAG_HIDDEN) == !visible &&
           lv_obj_has_flag(refresh, LV_OBJ_FLAG_HIDDEN) == !visible &&
           lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN) == !visible &&
           lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN) == (!visible || home_route) &&
           "header targets must only be active on the dashboard");
    assert(!find_clickable_rect(screen, 280, 0, 72, 56) &&
           "the former digital-clock area has no clickable hitbox");
}

static void assert_clock_mascot_hidden(void)
{
    lv_obj_t *mascot = header_mascot();
    assert(lv_obj_has_flag(mascot, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(mascot) == 60 && lv_obj_get_y(mascot) == 0 &&
           "hidden mascot must stay in its resting pose");
    assert(lv_obj_get_y(lv_obj_get_child(mascot, 3)) == 14 &&
           lv_obj_get_y(lv_obj_get_child(mascot, 4)) == 14 &&
           lv_obj_get_y(lv_obj_get_child(mascot, 5)) == 32 &&
           lv_obj_get_y(lv_obj_get_child(mascot, 7)) == 32 &&
           "hidden mascot limbs must stay at rest");
    for (int y = 0; y < 56; ++y)
        for (int x = 68; x < 128; ++x)
            assert(!pixel_is_orange(x, y) && "clock must not render mascot pixels");
}

static void assert_clock_view(void)
{
    assert_no_visible_digital_clock();
    assert(!lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN));
    assert_black_corners();
    assert_clock_mascot_hidden();
    int lit_pixels = 0;
    int min_x = WIDTH, min_y = HEIGHT, max_x = -1, max_y = -1;
    const lv_color_t lit = lv_color_hex(0xF9F6EF);
    const lv_color_t black = lv_color_hex(0x000000);
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) {
            const bool dot = (x >= 12 && x < 22 || x >= 458 && x < 468) &&
                             (y >= 12 && y < 22 || y >= 458 && y < 468);
            if (dot) continue;
            lit_pixels += frame[y * WIDTH + x].full == lit.full;
            if (frame[y * WIDTH + x].full == black.full) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    assert(lit_pixels > 100 && "visible word-clock letters must actually repaint bright");
    assert(min_x == 80 && max_x == 398 && min_y == 80 && max_y == 398 &&
           "clock ink must occupy the intended 319-pixel square without clipping");
    const int left_gap = min_x - 22, right_gap = 458 - max_x - 1;
    const int top_gap = min_y - 22, bottom_gap = 458 - max_y - 1;
    const int old_left_gap = 40 - 22, old_right_gap = 458 - 438 - 1;
    const int old_top_gap = 40 - 22, old_bottom_gap = 458 - 438 - 1;
    assert(left_gap >= 3 * old_left_gap && top_gap >= 3 * old_top_gap &&
           "top-left dot must have triple the previous blank axis clearance");
    assert(right_gap >= 3 * old_right_gap && top_gap >= 3 * old_top_gap &&
           "top-right dot must have triple the previous blank axis clearance");
    assert(right_gap >= 3 * old_right_gap && bottom_gap >= 3 * old_bottom_gap &&
           "bottom-right dot must have triple the previous blank axis clearance");
    assert(left_gap >= 3 * old_left_gap && bottom_gap >= 3 * old_bottom_gap &&
           "bottom-left dot must have triple the previous blank axis clearance");
    assert(abs((max_x - min_x) - (max_y - min_y)) <= 2 &&
           "visible clock letter field must be square");
    assert(abs(min_x - (WIDTH - 1 - max_x)) <= 2 &&
           abs(min_y - (HEIGHT - 1 - max_y)) <= 2 &&
           "clock ink bounds must be optically centered");
}

static void assert_board_view(void)
{
    if (!lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN))
        fprintf(stderr, "dashboard expected at scenario=%u ms; word-clock layer is visible\n",
                scenario_elapsed_ms);
    ++board_assert_count;
    assert(lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN));
    lv_refr_now(NULL);
    lv_obj_t *home = find_screen_box(0, 56, 480, 424);
    lv_obj_t *content = find_screen_box(24, 90, 432, 344);
    assert(((home && !lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN)) ||
            (content && !lv_obj_has_flag(content, LV_OBJ_FLAG_HIDDEN))) &&
           "normal focus must show a Home widget or one provider detail");
    assert_flat_dashboard_pixels(0);
}

static void assert_umlaut_font(uint32_t codepoint)
{
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(&usage_ui_font, &glyph, codepoint, 0));
    assert(!glyph.is_placeholder && glyph.resolved_font == &usage_ui_font);
    assert(glyph.box_w > 0 && glyph.box_h > 0 && glyph.bpp > 0);
    const uint8_t *bitmap = lv_font_get_glyph_bitmap(&usage_ui_font, codepoint);
    assert(bitmap);
    size_t bytes = ((size_t)glyph.box_w * glyph.box_h * glyph.bpp + 7) / 8;
    bool ink = false;
    for (size_t i = 0; i < bytes; ++i) ink |= bitmap[i] != 0;
    assert(ink && "umlaut glyph bitmap must contain ink");
}

static void assert_lit_word(int row, int column, int length)
{
    lv_obj_t *layer = word_layer();
    for (int col = column; col < column + length; ++col) {
        lv_obj_t *cell = lv_obj_get_child(layer, row * WORD_CLOCK_COLS + col);
        assert(lv_obj_get_style_text_color(cell, LV_PART_MAIN).full ==
               lv_color_hex(0xF9F6EF).full);
    }
}

static void assert_lit_umlaut_pixel(int row, int col)
{
    lv_obj_t *cell = lv_obj_get_child(word_layer(), row * WORD_CLOCK_COLS + col);
    assert(lv_obj_get_style_text_color(cell, LV_PART_MAIN).full ==
           lv_color_hex(0xF9F6EF).full);
    const lv_color_t lit = lv_color_hex(0xF9F6EF);
    int pixels = 0;
    for (int y = lv_obj_get_y(cell); y < lv_obj_get_y(cell) + 36; ++y)
        for (int x = lv_obj_get_x(cell); x < lv_obj_get_x(cell) + 40; ++x)
            pixels += frame[y * WIDTH + x].full == lit.full;
    assert(pixels > 0 && "lit umlaut must appear in the actual framebuffer");
}

static void assert_word_render(int hour, int minute, bool ready)
{
    static const lv_point_t dot_positions[4] = {
        {12, 12}, {458, 12}, {458, 458}, {12, 458},
    };
    lv_obj_t *layer = word_layer();
    word_clock_state_t expected;
    word_clock_compute(hour, minute, ready, &expected);
    assert(lv_obj_get_child_cnt(layer) == WORD_CLOCK_CELLS + 5);
    assert_umlaut_font(0x00DC); /* Ü */
    assert_umlaut_font(0x00D6); /* Ö */
    for (int row = 0; row < WORD_CLOCK_ROWS; row++) {
        assert(_lv_txt_get_encoded_length(word_clock_rows[row]) == WORD_CLOCK_COLS);
        const char *cursor = word_clock_rows[row];
        size_t remaining = strlen(cursor);
        for (int col = 0; col < WORD_CLOCK_COLS; col++) {
            int index = row * WORD_CLOCK_COLS + col;
            lv_obj_t *cell = lv_obj_get_child(layer, index);
            const char *letter = lv_label_get_text(cell);
            assert(remaining > 0);
            size_t bytes = _lv_txt_encoded_size(cursor);
            assert(bytes > 0 && bytes <= 4 && bytes <= remaining);
            assert(lv_obj_check_type(cell, &lv_label_class) &&
                   strlen(letter) == bytes && memcmp(letter, cursor, bytes) == 0);
            cursor += bytes;
            remaining -= bytes;
            assert(lv_obj_get_x(cell) == 68 + (col * 304 + 5) / 10 &&
                   lv_obj_get_y(cell) == 80 + (row * 298 + 4) / 9 &&
                   lv_obj_get_width(cell) == 40);
            assert(lv_obj_get_style_text_font(cell, LV_PART_MAIN) == &usage_ui_font);
            assert(lv_obj_get_style_text_color(cell, LV_PART_MAIN).full ==
                   lv_color_hex(expected.active[index] ? 0xF9F6EF : 0x181818).full);
        }
        assert(remaining == 0);
    }
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_get_child(layer, WORD_CLOCK_CELLS + i);
        assert(lv_obj_get_x(dot) == dot_positions[i].x &&
               lv_obj_get_y(dot) == dot_positions[i].y);
        assert(lv_obj_get_width(dot) == 10 && lv_obj_get_height(dot) == 10);
        assert(lv_obj_get_style_radius(dot, LV_PART_MAIN) == LV_RADIUS_CIRCLE);
        lv_color_t color = lv_color_hex(i < expected.minute_dots ? 0xF9F6EF : 0x181818);
        assert(lv_obj_get_style_bg_color(dot, LV_PART_MAIN).full == color.full);
        if (!lv_obj_has_flag(layer, LV_OBJ_FLAG_HIDDEN)) {
            const int x = dot_positions[i].x, y = dot_positions[i].y;
            assert(frame[(y + 5) * WIDTH + x + 5].full == color.full &&
                   "minute dot center must render in its corner");
            assert(frame[y * WIDTH + x].full == lv_color_hex(0x000000).full &&
                   frame[y * WIDTH + x + 9].full == lv_color_hex(0x000000).full &&
                   frame[(y + 9) * WIDTH + x].full == lv_color_hex(0x000000).full &&
                   frame[(y + 9) * WIDTH + x + 9].full == lv_color_hex(0x000000).full &&
                   "minute dots must have round, black corners");
        }
    }
    lv_obj_t *wait = lv_obj_get_child(layer, WORD_CLOCK_CELLS + 4);
    assert(lv_obj_has_flag(wait, LV_OBJ_FLAG_HIDDEN) == expected.valid);
}

static void clock_scene(const char *scene, const char *path)
{
    int hour = 10, minute = 8;
    if (strcmp(scene, "clock-five") == 0) minute = 5;
    if (strcmp(scene, "clock-quarter") == 0) minute = 15;
    if (strcmp(scene, "clock-half") == 0) minute = 30;
    if (strcmp(scene, "clock-midnight") == 0) { hour = 0; minute = 0; }
    if (strcmp(scene, "clock-rollover") == 0) { hour = 10; minute = 58; }
    if (strcmp(scene, "clock-unready") == 0) {
        box_settings_t settings;
        box_config_defaults(&settings);
        settings.providers = 2;
        settings.default_view = 0;
        box_config_set_current(&settings);
        display_init();
        example_usage();
        step_ms(5000);
        step_ms(31000); /* idle saver lease starts when the boot face closes */
        assert(!lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN));
        assert_black_corners();
        assert_clock_mascot_hidden();
        assert_word_render(-1, -1, false);
        assert(!contains_label(lv_scr_act(), "Waiting for time") &&
               "the clock saver remains a clean word grid while time is unready");
        assert_no_visible_digital_clock();
        step_ms(60000);
        assert(!lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN));
        assert_word_render(-1, -1, false);
        save_ppm(path);
        return;
    }
    display_init();
    set_berlin_time(hour, minute);
    display_clock_ready();
    example_usage();
    step_ms(5000);
    assert_board_view();
    step_ms(29000);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();
    assert_word_render(hour, minute, true);
    if (strcmp(scene, "clock-five") == 0) {
        assert_lit_word(0, 7, 4); /* FÜNF */
        assert_lit_word(3, 7, 4); /* NACH */
        assert_lit_word(9, 0, 4); /* ZEHN */
        assert_lit_umlaut_pixel(0, 8);
    }
    if (strcmp(scene, "clock-quarter") == 0) {
        assert_lit_word(2, 4, 7); /* VIERTEL */
        assert_lit_word(3, 7, 4); /* NACH */
        assert_lit_word(9, 0, 4); /* ZEHN */
    }
    if (strcmp(scene, "clock-half") == 0) {
        assert_lit_word(4, 0, 4); /* HALB */
        assert_lit_word(4, 5, 3); /* ELF */
    }
    if (strcmp(scene, "clock-midnight") == 0) {
        assert_lit_word(8, 6, 5); /* ZWÖLF */
        assert_lit_word(9, 8, 3); /* UHR */
        assert_lit_umlaut_pixel(8, 8);
    }
    if (strcmp(scene, "clock-rollover") == 0) {
        assert_lit_word(0, 7, 4); /* FÜNF */
        assert_lit_word(3, 0, 3); /* VOR */
        assert_lit_word(4, 5, 3); /* ELF */
    }
    if (strcmp(scene, "clock-normal") == 0) {
        assert(hour == 10 && minute == 8);
        const lv_point_t corners[4] = {{12, 12}, {458, 12}, {458, 458}, {12, 458}};
        for (int i = 0; i < 4; ++i)
            assert(frame[(corners[i].y + 5) * WIDTH + corners[i].x + 5].full ==
                   lv_color_hex(i < 3 ? 0xF9F6EF : 0x181818).full &&
                   "10:08 must light TL, TR, BR and leave BL dark");
        step_ms(8000);
        assert_clock_view();
        assert_word_render(hour, minute, true);
    }
    if (strcmp(scene, "clock-rollover") == 0) {
        set_berlin_time(11, 0);
        step_ms(1000);
        assert_word_render(11, 0, true);
        assert_lit_word(4, 5, 3); /* ELF */
        assert_lit_word(9, 8, 3); /* UHR */
    }
    save_ppm(path);
}

static void clock_cycle_scene(const char *base)
{
    display_init();
    set_berlin_time(10, 8);
    display_clock_ready();
    example_usage();
    step_ms(5000);
    assert_board_view();
    step_ms(29000);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();
    save_named_frame(base, "clock");

    display_set_refresh_callback(mock_refresh, &refresh_requests);
    pointer_tap(240, 430); /* Former footer region is a reveal tap on the clock. */
    assert_board_view();
    assert(!lv_obj_has_flag(header_mascot(), LV_OBJ_FLAG_HIDDEN) &&
           "clock tap must restore the mascot on the dashboard");
    lv_refr_now(NULL);
    assert(pixel_is_orange(80, 12) && "restored dashboard mascot must render");
    step_ms(1200);
    step_ms(1400);
    assert(lv_obj_get_x(header_mascot()) > 60 &&
           "mascot animation must restart after leaving the clock");
    assert(refresh_requests == 0 && "clock reveal must use cached data, not HTTP");
    step_ms(26400);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();
    pointer_tap(0, 0);
    assert_board_view();
    assert(refresh_requests == 0 && "clock corner tap must not start a request");
    step_ms(30000);
    assert_clock_view();

    /* The scheduled poll starts on the clock and may outlive the five-second face. */
    display_refresh_cycle_begin();
    display_updating();
    step_ms(320);
    assert(pixel_is_orange(10, 10));
    assert(refresh_requests == 0 && "automatic polling bypasses the touch callback");
    save_named_frame(base, "face");
    step_ms(4680);
    assert(!pixel_is_orange(10, 10));
    assert(contains_label(lv_scr_act(), "Updating..."));
    step_ms(60000);
    assert_board_view();
    assert(contains_label(lv_scr_act(), "Updating..."));
    pointer_tap(240, 430);
    assert(refresh_requests == 0 && "busy footer taps cannot queue a second poll");
    example_usage();
    display_refresh_cycle_end();
    assert_board_view();
    step_ms(29000);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();

    /* A manual refresh begins only after the revealed dashboard receives a new tap. */
    pointer_tap(479, 479);
    assert_board_view();
    assert(refresh_requests == 0);
    pointer_tap(240, 430);
    assert(refresh_requests == 1);
    pointer_tap(240, 430);
    assert(refresh_requests == 1 && "accepted request remains gated until cycle begin");
    display_refresh_cycle_begin();
    display_updating();
    step_ms(100);
    example_usage(); /* Quick HTTP response arrives while the face is still visible. */
    display_refresh_cycle_end();
    step_ms(4900);
    assert_board_view();
    save_named_frame(base, "dashboard");
    step_ms(29000);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();
    assert(refresh_requests == 1);
    save_named_frame(base, "return");
}

static void header_actions_scene(const char *base)
{
    box_settings_t settings;
    box_config_defaults(&settings);
    settings.providers = 2;
    settings.default_view = 0;
    settings.refresh_animation = true;
    settings.mascot_motion = false;
    box_config_set_current(&settings);
    display_init();
    set_berlin_time(10, 8);
    display_clock_ready();
    example_usage();
    step_periodic_ms(5000);
    assert_board_view();
    assert_no_visible_digital_clock();
    display_set_refresh_callback(mock_refresh, &refresh_requests);
    assert_header_targets(true);
    save_named_frame(base, "dashboard");

    /* Former header clock location is inert; screensaver begins only after idle. */
    pointer_tap(316, 24);
    assert_board_view();
    assert_no_visible_digital_clock();
    assert(refresh_requests == 0);

    /* Clawd still starts its large wink, with no small digital time over it. */
    pointer_tap(88, 24);
    scenario_elapsed_ms = 0; /* Start phase timing after the pointer tap latency. */
    step_until(320);
    assert_face_open();
    save_named_frame(base, "mascot-open");
    step_until(1560);
    step_until(1840);
    step_until(3000);
    assert_face_pixel(160, 190, 0xD97757);
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 190, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_no_visible_digital_clock();
    save_named_frame(base, "mascot-wink");
    step_until(5000);
    assert_board_view();
    assert_no_visible_digital_clock();

    step_periodic_ms(31000); /* Allow the next one-second refresh tick after deadline. */
    assert_clock_view();
    assert_word_render(10, 8, true);
    assert_no_visible_digital_clock();
    save_named_frame(base, "automatic-saver");
}

static void assert_attention_for_60s(const char *prefix)
{
    lv_refr_now(NULL);
    assert_board_view();
    assert(contains_label(lv_scr_act(), prefix));
    step_ms(60000);
    assert_board_view();
    assert(contains_label(lv_scr_act(), prefix));
}

static void clock_attention_scene(const char *path)
{
    display_init();
    set_berlin_time(10, 8);
    display_clock_ready();
    example_usage();
    step_ms(5000);
    step_ms(30000);
    assert_clock_view();

    step_ms(901000);
    assert_attention_for_60s("Stale");
    example_usage();
    step_ms(30000);
    assert_clock_view();

    display_usage(NAN, NULL, NAN, NULL);
    assert_attention_for_60s("Unknown");
    example_usage();
    step_ms(30000);
    assert_clock_view();

    const struct { const char *title, *detail, *prefix; } cases[] = {
        {"Usage", "Connection failed; retry later", "Error:"},
        {"Wi-Fi", "Connection failed; retrying", "Offline:"},
        {"Time", "Waiting for clock", "Waiting:"},
        {"Usage", "Retrying after cooldown", "Retry:"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        display_status(cases[i].title, cases[i].detail);
        assert_attention_for_60s(cases[i].prefix);
        example_usage();
        step_ms(5000); /* Recovery face must finish before the dashboard deadline. */
        assert_board_view();
        step_ms(29000);
        assert_board_view();
        step_ms(1000);
        assert_clock_view();
    }

    time_t valid_time = mock_epoch;
    mock_epoch = 0;
    step_ms(1000);
    assert_attention_for_60s("Waiting for time");
    assert_word_render(-1, -1, false);
    mock_epoch = valid_time;
    step_ms(1000);
    example_usage();
    step_ms(30000);
    assert_clock_view();

    display_setup("http://192.168.4.1/setup");
    step_ms(60000);
    assert(lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN));
    assert(contains_label(lv_scr_act(), "Connect Claude"));
    display_set_device_name("hello-box-4827");
    assert(contains_label(lv_scr_act(), "hello-box-4827"));
    display_set_refresh_callback(mock_refresh, &refresh_requests);
    pointer_tap(240, 430);
    assert(refresh_requests == 0 && "setup QR must not forward footer taps");
    example_usage();
    display_set_device_name("hello-box-9371");
    assert(!contains_label(lv_scr_act(), "Connect Claude") &&
           "setup content must be destroyed before a later name update");
    step_ms(320);
    assert(pixel_is_orange(10, 10) && "first post-setup usage needs the face");
    step_ms(4680);
    assert_board_view();
    step_ms(29000);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();
    assert_word_render(10, 8, true);
    save_ppm(path);
}

static lv_obj_t *overlay_for_title(const char *title)
{
    lv_obj_t *screen = lv_scr_act();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(screen); ++i) {
        lv_obj_t *overlay = lv_obj_get_child(screen, i);
        if (lv_obj_get_width(overlay) != WIDTH || lv_obj_get_height(overlay) != HEIGHT ||
            lv_obj_get_child_cnt(overlay) < 3) continue;
        lv_obj_t *heading = lv_obj_get_child(overlay, 0);
        if (lv_obj_check_type(heading, &lv_label_class) &&
            strcmp(lv_label_get_text(heading), title) == 0) return overlay;
    }
    assert(!"QR overlay heading must exist");
    return NULL;
}

static void assert_overlay_qr_at(lv_obj_t *overlay, bool shown, int field_x, int field_y)
{
    lv_obj_t *field = lv_obj_get_child(overlay, 1);
    lv_obj_t *qr = lv_obj_get_child(field, 0);
    if (lv_obj_has_flag(field, LV_OBJ_FLAG_HIDDEN) != !shown) {
        fprintf(stderr, "QR visibility mismatch: expected shown=%d, field hidden=%d, detail=%s\n",
                shown, lv_obj_has_flag(field, LV_OBJ_FLAG_HIDDEN),
                lv_label_get_text(lv_obj_get_child(overlay, 2)));
        abort();
    }
    assert(lv_obj_get_x(field) == field_x && lv_obj_get_y(field) == field_y);
    assert(lv_obj_get_width(field) == 252 && lv_obj_get_height(field) == 252);
    assert(lv_obj_get_x(qr) == 30 && lv_obj_get_y(qr) == 30);
    assert(lv_obj_get_width(qr) == 192 && lv_obj_get_height(qr) == 192);
    lv_refr_now(NULL);
    if (shown) {
        const lv_color_t white = lv_color_hex(0xFFFFFF);
        assert(frame[(field_y + 4) * WIDTH + field_x + 4].full == white.full &&
               frame[(field_y + 247) * WIDTH + field_x + 247].full == white.full &&
               "QR quiet zone must remain white around the code");
        int dark = 0, light = 0;
        for (int y = field_y + 30; y < field_y + 222; ++y)
            for (int x = field_x + 30; x < field_x + 222; ++x) {
                dark += frame[y * WIDTH + x].full == lv_color_hex(0x000000).full;
                light += frame[y * WIDTH + x].full == white.full;
            }
        assert(dark > 1000 && light > 1000 && "QR must contain visible modules");
    }
}

static void assert_overlay_qr(lv_obj_t *overlay, bool shown)
{
    assert_overlay_qr_at(overlay, shown, 114, 99);
}

static void assert_wifi_steps(lv_obj_t *wifi, bool phone_ready)
{
    lv_refr_now(NULL);
    lv_obj_t *step1 = find_label(wifi, "1. Connect to this Wi-Fi");
    lv_obj_t *step2 = find_label(wifi, "2. Open the setup page");
    lv_obj_t *title = find_label(wifi, "Wi-Fi setup");
    lv_obj_t *done = find_label(wifi, "Done");
    assert(title && step1 && step2 && done &&
           "Wi-Fi title, both steps, and completion marker stay visible");
    assert(lv_obj_get_x(title) == 24 && lv_obj_get_y(title) == 12 &&
           lv_obj_get_width(title) == 432 &&
           lv_obj_get_style_text_font(title, LV_PART_MAIN) == &usage_ui_font);
    assert(lv_obj_get_x(step1) == 40 && lv_obj_get_y(step1) == 48 &&
           lv_obj_get_width(step1) == 328);
    assert(lv_obj_get_x(step2) == 40 && lv_obj_get_y(step2) == 77 &&
           lv_obj_get_width(step2) == 328);
    assert(lv_obj_get_style_text_font(step1, LV_PART_MAIN) == &usage_status_font &&
           lv_obj_get_style_text_font(step2, LV_PART_MAIN) == &usage_status_font);
    assert(lv_obj_get_x(done) == 371 && lv_obj_get_y(done) == 48);
    assert(lv_obj_has_flag(done, LV_OBJ_FLAG_HIDDEN) == !phone_ready);
    const uint32_t step1_text = phone_ready ? 0xDDDCD6 : 0xF9F6EF;
    const uint32_t step2_text = phone_ready ? 0xF9F6EF : 0xA6A49F;
    const uint32_t step1_mark = phone_ready ? 0xB3C84D : 0xD97757;
    const uint32_t step2_mark = phone_ready ? 0xD97757 : 0xA6A49F;
    assert(lv_obj_get_style_text_color(step1, LV_PART_MAIN).full ==
           lv_color_hex(step1_text).full);
    assert(lv_obj_get_style_text_color(step2, LV_PART_MAIN).full ==
           lv_color_hex(step2_text).full);
    lv_obj_t *mark1 = NULL, *mark2 = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(wifi); ++i) {
        lv_obj_t *child = lv_obj_get_child(wifi, i);
        if (lv_obj_get_x(child) == 24 && lv_obj_get_y(child) == 48 &&
            lv_obj_get_width(child) == 4 && lv_obj_get_height(child) == 19) mark1 = child;
        if (lv_obj_get_x(child) == 24 && lv_obj_get_y(child) == 77 &&
            lv_obj_get_width(child) == 4 && lv_obj_get_height(child) == 19) mark2 = child;
    }
    assert(mark1 && mark2 && "step markers remain aligned with their labels");
    assert(lv_obj_get_style_bg_color(mark1, LV_PART_MAIN).full ==
           lv_color_hex(step1_mark).full);
    assert(lv_obj_get_style_bg_color(mark2, LV_PART_MAIN).full ==
           lv_color_hex(step2_mark).full);
    const lv_color_t background = lv_color_hex(0x151A17);
    int title_ink = 0, step_ink = 0;
    for (int y = 12; y < 42; ++y)
        for (int x = 24; x < 456; ++x)
            title_ink += frame[y * WIDTH + x].full != background.full;
    for (int y = 48; y < 96; ++y)
        for (int x = 24; x < 368; ++x)
            step_ink += frame[y * WIDTH + x].full != background.full;
    assert(title_ink > 20 && "Wi-Fi title must be visible in the framebuffer");
    assert(step_ink > 40 && "both Wi-Fi step rows must be visible in the framebuffer");
    assert(frame[55 * WIDTH + 25].full == lv_color_hex(step1_mark).full &&
           frame[84 * WIDTH + 25].full == lv_color_hex(step2_mark).full &&
           "active/completed step markers must be visible in the framebuffer");
}

static void assert_device_name_pixels(void)
{
    lv_refr_now(NULL);
    int ink = 0;
    const lv_color_t black = lv_color_hex(0x000000);
    for (int y = 70; y < 90; ++y)
        for (int x = 150; x < 330; ++x)
            ink += frame[y * WIDTH + x].full != black.full;
    assert(ink > 20 && "visible device name must render above the QR field");
}

static void assert_wifi_name(lv_obj_t *wifi, const char *identity)
{
    lv_obj_t *name = find_label(wifi, identity);
    assert(name && "current hello-box SSID must be visible");
    assert(lv_obj_get_x(name) == 24 && lv_obj_get_y(name) == 373 &&
           lv_obj_get_width(name) == 432);
    assert(lv_obj_get_style_text_font(name, LV_PART_MAIN) == &usage_ui_font);
    lv_refr_now(NULL);
    int ink = 0;
    const lv_color_t background = lv_color_hex(0x151A17);
    for (int y = 373; y < 403; ++y)
        for (int x = 24; x < 456; ++x)
            ink += frame[y * WIDTH + x].full != background.full;
    assert(ink > 20 && "visible device SSID must render in its advertised region");
}

static void assert_wifi_copy(lv_obj_t *wifi, bool phone_ready, const char *message)
{
    lv_obj_t *hint = find_label(wifi, phone_ready ? "Scan QR to open page" :
                                                     "Scan QR to join Wi-Fi");
    lv_obj_t *detail = lv_obj_get_child(wifi, 2);
    assert(hint && lv_obj_check_type(detail, &lv_label_class) &&
           "scan hint and detail label are present");
    if (strlen(message) <= 42)
        assert(strcmp(lv_label_get_text(detail), message) == 0 &&
               "short backend message remains visible verbatim");
    assert(lv_obj_get_x(hint) == 24 && lv_obj_get_y(hint) == 402 &&
           lv_obj_get_width(hint) == 432);
    assert(lv_obj_get_x(detail) == 24 && lv_obj_get_y(detail) == 427 &&
           lv_obj_get_width(detail) == 432 && lv_obj_get_height(detail) == 42);
    assert(lv_obj_get_style_text_font(detail, LV_PART_MAIN) == &usage_status_font);
    if (strlen(message) > 42) {
        lv_refr_now(NULL);
        int ink = 0;
        const lv_color_t background = lv_color_hex(0x151A17);
        for (int y = 427; y < 469; ++y)
            for (int x = 24; x < 456; ++x)
                ink += frame[y * WIDTH + x].full != background.full;
        assert(ink > 20 && "long status text must remain visible within the bounded detail label");
    }
}

static void settings_flow_scene(const char *base)
{
    display_init();
    char identity[] = "hello-box-4827";
    display_set_device_name(identity);
    identity[10] = '9'; /* Display must own the string. */
    display_clock_ready();
    settle_boot();
    example_usage();
    display_set_refresh_callback(mock_refresh, &refresh_requests);
    display_set_settings_callback(mock_settings, &settings_opens);
    lv_obj_t *settings_label = find_label(lv_scr_act(), LV_SYMBOL_SETTINGS);
    assert(settings_label && lv_obj_get_x(settings_label) >= 406 &&
           lv_obj_get_y(settings_label) >= 0 && lv_obj_get_y(settings_label) < 56);
    pointer_tap(407, 24);
    assert(settings_opens == 0 && refresh_requests == 0);
    pointer_tap(356, 0);
    assert(refresh_requests == 1 && settings_opens == 0 &&
           "refresh begins exactly after the Settings hitbox");
    display_refresh_cycle_begin();
    display_refresh_cycle_end();
    step_ms(5000);
    pointer_tap(430, 24);
    assert(settings_opens == 1 && refresh_requests == 1 && last_settings_id != 0);
    uint32_t first_id = last_settings_id;
    lv_obj_t *settings = overlay_for_title("Settings");
    assert(!lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    assert(find_label(settings, "hello-box-4827"));
    display_set_device_name("<script>alert(1)</script>");
    assert(find_label(settings, "hello-box-4827"));
    assert(contains_label(settings, "Opening..."));
    assert_overlay_qr(settings, false);
    save_named_frame(base, "pending");
    pointer_tap(200, 440);
    assert(settings_opens == 1 && refresh_requests == 1);
    display_settings_qr(first_id, "http://192.168.1.12/s/demo-capability");
    assert_overlay_qr(settings, true);
    pointer_tap(88, 24);
    pointer_tap(320, 24);
    assert(!lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN) && settings_opens == 1 &&
           refresh_requests == 1 && "Settings overlay must guard both header actions");
    display_set_device_name("hello-box-9371");
    assert(find_label(settings, "hello-box-9371"));
    assert_overlay_qr(settings, true);
    display_set_device_name("hello-box-4827");
    assert(find_label(settings, "hello-box-4827"));
    assert_overlay_qr(settings, true);
    save_named_frame(base, "qr");
    display_refresh_cycle_begin();
    display_updating();
    step_ms(320);
    assert_overlay_qr(settings, true);
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    display_refresh_cycle_end();
    step_ms(5000);
    assert_overlay_qr(settings, true);
    display_refresh_cycle_begin();
    display_updating();
    step_ms(6000);
    assert_overlay_qr(settings, true);
    display_refresh_cycle_end();
    step_ms(35000);
    assert(!lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN) &&
           "Settings must suspend the usage clock timeout");
    display_usage(25.0f, "2026-10-26T23:00:00Z", 65.0f, "2026-11-02T20:00:00Z");
    display_status("Usage", "Temporary error");
    assert(!lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    assert_overlay_qr(settings, true);
    display_settings_closed(first_id + 1);
    assert(!lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    pointer_tap(60, 440);
    assert(settings_closes == 1 && last_closed_id == first_id);
    assert(lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    assert(contains_label(lv_scr_act(), "75%") &&
           "Back must retain the latest usage that arrived under Settings");
    display_settings_qr(first_id, "http://192.168.1.12/s/old");
    assert(lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    display_usage(25.0f, "2026-10-26T23:00:00Z", 65.0f, "2026-11-02T20:00:00Z");
    step_ms(5000); /* Recovery face finishes before Settings becomes tappable. */
    pointer_tap(430, 24);
    assert(settings_opens == 2 && last_settings_id != first_id);
    uint32_t second_id = last_settings_id;
    display_settings_qr(first_id, "http://192.168.1.12/s/stale");
    assert_overlay_qr(settings, false);
    display_settings_qr(second_id, "http://192.168.1.12/s/new");
    assert_overlay_qr(settings, true);
    settings_close_accept = false;
    pointer_tap(60, 440);
    assert(settings_closes == 2 && last_closed_id == second_id);
    assert(!lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN) &&
           "rejected Back must keep the QR visible during durable commit");
    display_settings_closed(second_id);
    assert(lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    settings_close_accept = true;
    display_settings_qr(second_id, "http://192.168.1.12/s/late");
    assert(lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    settings_accept = false;
    pointer_tap(430, 24);
    assert(settings_opens == 3);
    assert(!lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    assert(contains_label(settings, "Settings unavailable"));
    assert_overlay_qr(settings, false);
    save_named_frame(base, "unavailable");
    display_settings_qr(last_settings_id, "http://192.168.1.12/s/rejected");
    assert_overlay_qr(settings, false);
    settings_close_accept = false;
    pointer_tap(60, 440);
    assert(lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    settings_close_accept = true;
    step_ms(29000);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();
    save_named_frame(base, "return");
}

static void wifi_flow_scene(const char *base)
{
    const char *identity = "hello-box-4827";
    char join_qr[80];
    int qr_length = snprintf(join_qr, sizeof(join_qr),
                             "WIFI:T:WPA;S:%s;P:demo-pass123;;", identity);
    assert(qr_length > 0 && qr_length < (int)sizeof(join_qr));
    display_init();
    display_set_device_name(identity);
    display_clock_ready();
    settle_boot();
    example_usage();
    display_set_settings_callback(mock_settings, &settings_opens);
    pointer_tap(430, 24);
    assert(settings_opens == 1);
    display_wifi_setup(join_qr,
                       "http://192.168.4.1/setup", false, NULL);
    assert(settings_closes == 0 &&
           "Wi-Fi takes visual priority without sending a second CLOSE callback");
    lv_obj_t *settings = overlay_for_title("Settings");
    lv_obj_t *wifi = overlay_for_title("Wi-Fi setup");
    assert(lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN));
    assert_wifi_name(wifi, "hello-box-4827");
    assert_wifi_steps(wifi, false);
    assert_overlay_qr_at(wifi, true, 114, 112);
    assert_wifi_copy(wifi, false, "Scan to join setup Wi-Fi");
    pointer_tap(88, 24);
    pointer_tap(320, 24);
    assert(!lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN) && settings_opens == 1 &&
           refresh_requests == 0 && "Wi-Fi overlay must guard both header actions");
    display_set_device_name("hello-box-9371");
    assert_wifi_name(wifi, "hello-box-9371");
    assert_overlay_qr_at(wifi, true, 114, 112);
    display_set_device_name("hello-box-4827");
    assert_wifi_name(wifi, "hello-box-4827");
    assert_overlay_qr_at(wifi, true, 114, 112);
    save_named_frame(base, "join");

    const char *testing = "Testing connection...";
    display_wifi_setup(join_qr, "http://192.168.4.1/setup", false, testing);
    assert_wifi_steps(wifi, false);
    assert_wifi_copy(wifi, false, testing);
    assert_overlay_qr_at(wifi, true, 114, 112);
    save_named_frame(base, "testing");

    const char *failure = "Connection failed. Check the Wi-Fi password and try again.";
    display_wifi_setup(join_qr, "http://192.168.4.1/setup", false, failure);
    assert_wifi_steps(wifi, false);
    assert_wifi_copy(wifi, false, failure);
    assert_overlay_qr_at(wifi, true, 114, 112);
    save_named_frame(base, "failure");

    const char *long_message = "Could not verify the connection yet. Keep this device near your hello-box, confirm that your phone joined its temporary Wi-Fi network, then scan again or restart setup if the problem continues.";
    display_wifi_setup(join_qr, "http://192.168.4.1/setup", false, long_message);
    assert_wifi_steps(wifi, false);
    assert_wifi_copy(wifi, false, long_message);
    assert_overlay_qr_at(wifi, true, 114, 112);
    save_named_frame(base, "long-message");

    display_updating();
    display_refresh_cycle_begin();
    step_ms(6000);
    assert(!lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN));
    assert_overlay_qr_at(wifi, true, 114, 112);
    assert_wifi_steps(wifi, false);
    display_wifi_setup(join_qr,
                       "http://192.168.4.1/setup", true, "Scan the page QR to continue.");
    assert_wifi_steps(wifi, true);
    assert_overlay_qr_at(wifi, true, 114, 112);
    assert_wifi_name(wifi, "hello-box-4827");
    assert_wifi_copy(wifi, true, "Scan the page QR to continue.");
    save_named_frame(base, "page");

    const char *saving = "Saving Wi-Fi settings...";
    display_wifi_setup(join_qr, "http://192.168.4.1/setup", true, saving);
    assert_wifi_steps(wifi, true);
    assert_wifi_copy(wifi, true, saving);
    assert_overlay_qr_at(wifi, true, 114, 112);
    save_named_frame(base, "saving");

    display_usage(30.0f, "2026-10-26T23:00:00Z", 70.0f, "2026-11-02T20:00:00Z");
    display_refresh_cycle_end();
    assert(!lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN));
    display_wifi_setup_done();
    assert(lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN));
    assert(contains_label(lv_scr_act(), "70%"));
    save_named_frame(base, "handoff");
    step_ms(29000);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();
}

static void assert_english_five_past_ten(void)
{
    const char *const *rows = word_clock_rows_for_language(WORD_CLOCK_ENGLISH);
    assert(strcmp(rows[0], "ITLISASTIME") == 0);
    assert(strcmp(rows[2], "TWENTYFIVEX") == 0);
    assert(strcmp(rows[4], "PASTERUNINE") == 0);
    assert(strcmp(rows[9], "TENSEOCLOCK") == 0);

    /* 10:08 must show IT IS FIVE PAST TEN, with three minute dots. */
    bool expected[WORD_CLOCK_CELLS] = {false};
    expected[0] = expected[1] = true;                 /* IT */
    expected[3] = expected[4] = true;                 /* IS */
    for (int col = 6; col < 10; ++col) expected[2 * WORD_CLOCK_COLS + col] = true; /* FIVE */
    for (int col = 0; col < 4; ++col) expected[4 * WORD_CLOCK_COLS + col] = true; /* PAST */
    for (int col = 0; col < 3; ++col) expected[9 * WORD_CLOCK_COLS + col] = true; /* TEN */

    lv_obj_t *layer = word_layer();
    assert(lv_obj_get_child_cnt(layer) >= WORD_CLOCK_CELLS + 4);
    const lv_color_t lit = lv_color_hex(0xF9F6EF);
    const lv_color_t dim = lv_color_hex(0x181818);
    for (int cell = 0; cell < WORD_CLOCK_CELLS; ++cell) {
        lv_obj_t *letter = lv_obj_get_child(layer, cell);
        assert(lv_obj_check_type(letter, &lv_label_class));
        const char *glyph = lv_label_get_text(letter);
        int row = cell / WORD_CLOCK_COLS, col = cell % WORD_CLOCK_COLS;
        char expected_glyph[2] = {rows[row][col], '\0'};
        assert(strcmp(glyph, expected_glyph) == 0 && "English clock cells must use English glyphs");
        assert(lv_obj_get_style_text_color(letter, LV_PART_MAIN).full ==
               (expected[cell] ? lit.full : dim.full) &&
               "English 10:08 phrase must light only IT IS FIVE PAST TEN");
    }
    for (int dot = 0; dot < 4; ++dot) {
        lv_obj_t *object = lv_obj_get_child(layer, WORD_CLOCK_CELLS + dot);
        assert(lv_obj_get_style_bg_color(object, LV_PART_MAIN).full ==
               (dot < 3 ? lit.full : dim.full) && "minute remainder must illuminate three corner dots");
    }
}

static void config_custom_scene(const char *base)
{
    box_settings_t settings;
    box_config_defaults(&settings);
    settings.clock_language = WORD_CLOCK_ENGLISH;
    settings.default_view = 1; /* Usage is the home screen. */
    settings.brightness = 61;
    settings.dashboard_timeout_sec = 10;
    settings.refresh_interval_sec = 60;
    settings.timezone = 5; /* Asia/Tokyo */
    settings.mascot_motion = false;
    settings.refresh_animation = false;
    box_config_set_current(&settings);

    display_init();
    assert(brightness_set_calls == 1 && brightness_percent == 61 &&
           "display startup must apply the configured brightness");
    assert(getenv("TZ") && strcmp(getenv("TZ"), "JST-9") == 0 &&
           "display startup must apply the configured timezone");
    lv_refr_now(NULL);
    assert(pixel_is_orange(10, 10) && "configuration must not disable the one-time boot face");
    save_named_frame(base, "boot-open");

    struct tm tokyo = {.tm_year = 125, .tm_mon = 9, .tm_mday = 26,
                       .tm_hour = 10, .tm_min = 8, .tm_isdst = -1};
    mock_epoch = mktime(&tokyo);
    display_clock_ready();
    settle_boot();
    assert_board_view();

    /* Usage-home ignores the configured 10-second temporary dashboard timeout. */
    lv_refr_now(NULL);
    lv_color_t mascot_rest[WIDTH * HEIGHT];
    memcpy(mascot_rest, frame, sizeof(mascot_rest));
    save_named_frame(base, "usage-home");
    step_ms(12000);
    assert_board_view();
    assert_mascot_region_equal(mascot_rest);

    /* A refresh cycle remains functional but the optional large face is off. */
    display_refresh_cycle_begin();
    step_ms(240);
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "refreshAnimation=false must suppress refresh face");
    assert_board_view();
    display_refresh_cycle_end();

    /* Clock tap opens the configured English face immediately. */
    pointer_tap(405, 40);
    assert(!lv_obj_has_flag(word_layer(), LV_OBJ_FLAG_HIDDEN));
    assert_black_corners();
    assert_clock_mascot_hidden();
    assert_english_five_past_ten();
    save_named_frame(base, "english-clock");

    /* Returning to the usage home does not start an auto-return timeout. */
    pointer_tap(220, 220);
    assert_board_view();
    step_ms(12000);
    assert_board_view();
    assert_mascot_region_equal(mascot_rest);
    save_named_frame(base, "usage-home-after-timeout");
    assert(write_ppm(base) == 0);
}

typedef struct {
    bool enabled;
    bool configured;
    unsigned refresh_calls;
} shell_test_app_t;

static bool shell_test_enabled(void *context)
{
    return ((shell_test_app_t *)context)->enabled;
}

static bool shell_test_configured(void *context)
{
    return ((shell_test_app_t *)context)->configured;
}

static bool shell_test_refresh(void *context)
{
    ++((shell_test_app_t *)context)->refresh_calls;
    return true;
}

static unsigned shell_view_expectation;

static void shell_expect_view(const ui_shell_t *shell, ui_route_t route,
                              app_id_t app_id, saver_id_t saver_id,
                              ui_overlay_t overlay)
{
    ++shell_view_expectation;
    ui_shell_view_t view = ui_shell_view(shell);
    if (view.route != route || view.app_id != app_id ||
        (route == UI_ROUTE_SAVER && view.saver_id != saver_id) ||
        view.overlay != overlay) {
        fprintf(stderr, "shell view #%u: got route=%d app=%u saver=%u overlay=%d; expected route=%d app=%u saver=%u overlay=%d\n",
                shell_view_expectation,
                view.route, (unsigned)view.app_id, (unsigned)view.saver_id, view.overlay,
                route, (unsigned)app_id, (unsigned)saver_id, overlay);
    }
    assert(view.route == route && view.app_id == app_id &&
           (route != UI_ROUTE_SAVER || view.saver_id == saver_id) &&
           view.overlay == overlay);
}

static void shell_policy_scene(const char *output)
{
    /* Exercise the pure shell state machine with the same registry types used by
     * the production display.  This catches route/timing regressions separately
     * from LVGL drawing while the interactive scenes below verify real pixels. */
    shell_test_app_t claude_state = {.enabled = true, .configured = true};
    shell_test_app_t codex_state = {.enabled = true, .configured = true};
    app_registry_t apps;
    app_registry_reset(&apps);
    app_descriptor_t claude = {.id = APP_ID_CLAUDE, .name = "Claude",
        .capabilities = APP_CAP_REFRESH | APP_CAP_INTERRUPT_SAVER,
        .context = &claude_state, .enabled = shell_test_enabled,
        .configured = shell_test_configured, .request_refresh = shell_test_refresh};
    app_descriptor_t codex = {.id = APP_ID_CODEX, .name = "Codex",
        .capabilities = APP_CAP_REFRESH | APP_CAP_INTERRUPT_SAVER,
        .context = &codex_state, .enabled = shell_test_enabled,
        .configured = shell_test_configured, .request_refresh = shell_test_refresh};
    assert(app_registry_register(&apps, &claude));
    assert(app_registry_register(&apps, &codex));
    assert(app_registry_count(&apps) == 2 && app_registry_at(&apps, 0)->id == APP_ID_CLAUDE &&
           app_registry_at(&apps, 1)->id == APP_ID_CODEX);

    screensaver_registry_t savers;
    screensaver_registry_reset(&savers);
    screensaver_descriptor_t clock = {.id = SAVER_ID_WORD_CLOCK, .name = "Word clock"};
    assert(screensaver_registry_register(&savers, &clock));
    assert(screensaver_registry_count(&savers) == 1);

    ui_shell_t shell;
    assert(ui_shell_init(&shell, &apps, &savers, 0, 30,
                         SAVER_ID_WORD_CLOCK, 0));
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    ui_shell_tick(&shell, 29999999);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    ui_shell_tick(&shell, 30000000);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);

    /* Two configured apps route fresh saver attention to a combined Home.  A
     * later sample and a duplicate never extend the original eight-second lease. */
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 10, 31000000);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    const int64_t lease_end = shell.attention_deadline_us;
    assert(lease_end == 39000000);
    ui_shell_fresh_sample(&shell, APP_ID_CODEX, 10, 35000000);
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 10, 38000000);
    assert(shell.attention_deadline_us == lease_end &&
           "subsequent and duplicate samples must not extend attention");
    ui_shell_tick(&shell, lease_end - 1);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    ui_shell_tick(&shell, lease_end);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);

    /* A single enabled provider gets its detail view. Disabled or unconfigured
     * apps cannot earn attention, even if a sample is pushed for them. */
    codex_state.enabled = false;
    assert(ui_shell_init(&shell, &apps, &savers, 0, 30,
                         SAVER_ID_WORD_CLOCK, 40000000));
    ui_shell_navigate_saver(&shell, SAVER_ID_WORD_CLOCK, 70000000);
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 11, 71000000);
    shell_expect_view(&shell, UI_ROUTE_APP, APP_ID_CLAUDE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    const int64_t one_app_end = shell.attention_deadline_us;
    codex_state.configured = false;
    ui_shell_fresh_sample(&shell, APP_ID_CODEX, 1, 72000000);
    assert(shell.attention_deadline_us == one_app_end);
    ui_shell_tick(&shell, one_app_end);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);

    /* A real user action during attention cancels the transient lease and
     * starts a fresh manual-idle window instead. */
    assert(ui_shell_init(&shell, &apps, &savers, 0, 30,
                         SAVER_ID_WORD_CLOCK, 90000000));
    ui_shell_navigate_saver(&shell, SAVER_ID_WORD_CLOCK, 120000000);
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 1, 121000000);
    shell_expect_view(&shell, UI_ROUTE_APP, APP_ID_CLAUDE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    ui_shell_user_activity(&shell, 122000000);
    assert(!shell.attention_active && shell.manual_deadline_us == 152000000);
    ui_shell_tick(&shell, 129000000); /* The old 8s attention deadline is ignored. */
    shell_expect_view(&shell, UI_ROUTE_APP, APP_ID_CLAUDE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    ui_shell_tick(&shell, 152000000);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);

    /* User navigation owns focus and resets a manual idle timer. Background
     * samples cannot steal the selected app or prolong that timer. */
    codex_state.enabled = true;
    codex_state.configured = true;
    assert(ui_shell_init(&shell, &apps, &savers, 0, 30,
                         SAVER_ID_WORD_CLOCK, 160000000));
    assert(ui_shell_navigate_app(&shell, APP_ID_CODEX, 160000000));
    const int64_t manual_end = shell.manual_deadline_us;
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 12, 161000000);
    shell_expect_view(&shell, UI_ROUTE_APP, APP_ID_CODEX, SAVER_ID_NONE, UI_OVERLAY_NONE);
    assert(shell.manual_deadline_us == manual_end);
    ui_shell_tick(&shell, manual_end);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);

    /* The face delays, but does not lose, a fresh-sample lease. */
    assert(ui_shell_init(&shell, &apps, &savers, 0, 30,
                         SAVER_ID_WORD_CLOCK, 100000000));
    ui_shell_navigate_saver(&shell, SAVER_ID_WORD_CLOCK, 130000000);
    ui_shell_set_face(&shell, true, 131000000);
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 13, 132000000);
    assert(shell.attention_pending && !shell.attention_active);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_FACE);
    ui_shell_set_face(&shell, false, 136000000);
    assert(shell.attention_active && !shell.attention_pending &&
           shell.attention_deadline_us == 144000000);

    /* Settings and Wi-Fi discard attention.  Closing either restores the
     * screensaver without replaying a result that arrived under the overlay. */
    ui_shell_set_settings(&shell, true, 137000000);
    ui_shell_fresh_sample(&shell, APP_ID_CODEX, 11, 138000000);
    assert(!shell.attention_active && !shell.attention_pending);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_SETTINGS);
    ui_shell_set_settings(&shell, false, 139000000);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);
    ui_shell_set_wifi(&shell, true, 140000000);
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 14, 141000000);
    ui_shell_set_wifi(&shell, false, 142000000);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    assert(shell.manual_deadline_us == 172000000 &&
           "finishing Wi-Fi setup gets a fresh manual Home window");
    ui_shell_tick(&shell, 171999999);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    ui_shell_tick(&shell, 172000000);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);
    ui_shell_set_boot(&shell, true, 173000000);
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 16, 174000000);
    assert(!shell.attention_active && !shell.attention_pending);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_BOOT);
    ui_shell_set_boot(&shell, false, 175000000);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);

    /* Refresh generations are app-scoped: stale ends do not finish a newer
     * cycle, and ending Claude cannot clear Codex or ALL. */
    ui_shell_refresh_begin(&shell, APP_ID_CLAUDE, 1, 150000000);
    ui_shell_refresh_begin(&shell, APP_ID_CODEX, 5, 150000000);
    assert(ui_shell_is_refreshing(&shell, APP_ID_CLAUDE));
    assert(ui_shell_is_refreshing(&shell, APP_ID_CODEX));
    assert(ui_shell_is_refreshing(&shell, APP_ID_ALL));
    ui_shell_refresh_begin(&shell, APP_ID_CODEX, 7, 151000000);
    ui_shell_refresh_end(&shell, APP_ID_CODEX, 5, 152000000);
    assert(ui_shell_is_refreshing(&shell, APP_ID_CODEX) &&
           "a stale generation end cannot finish the current app cycle");
    ui_shell_refresh_end(&shell, APP_ID_CLAUDE, 1, 153000000);
    assert(!ui_shell_is_refreshing(&shell, APP_ID_CLAUDE) &&
           ui_shell_is_refreshing(&shell, APP_ID_CODEX) &&
           ui_shell_is_refreshing(&shell, APP_ID_ALL));
    ui_shell_refresh_end(&shell, APP_ID_CODEX, 7, 154000000);
    assert(!ui_shell_is_refreshing(&shell, APP_ID_ALL));

    /* default_view=Home disables automatic saver entry; explicit saver stays
     * suppressed for only the generation that was already active. */
    assert(ui_shell_init(&shell, &apps, &savers, 1, 30,
                         SAVER_ID_WORD_CLOCK, 200000000));
    ui_shell_tick(&shell, 230000000);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);
    ui_shell_refresh_begin(&shell, APP_ID_CLAUDE, 9, 231000000);
    assert(ui_shell_navigate_saver(&shell, SAVER_ID_WORD_CLOCK, 232000000));
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 15, 233000000);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);
    ui_shell_refresh_end(&shell, APP_ID_CLAUDE, 9, 234000000);
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 15, 234500000);
    shell_expect_view(&shell, UI_ROUTE_SAVER, APP_ID_NONE,
                      SAVER_ID_WORD_CLOCK, UI_OVERLAY_NONE);
    ui_shell_refresh_begin(&shell, APP_ID_CLAUDE, 10, 235000000);
    ui_shell_fresh_sample(&shell, APP_ID_CLAUDE, 16, 236000000);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE,
                      SAVER_ID_NONE, UI_OVERLAY_NONE);
    ui_shell_tick(&shell, 300000000);
    shell_expect_view(&shell, UI_ROUTE_HOME, APP_ID_NONE, SAVER_ID_NONE, UI_OVERLAY_NONE);

    assert(app_registry_request_refresh(&apps, APP_ID_CLAUDE));
    assert(claude_state.refresh_calls == 1 && codex_state.refresh_calls == 0 &&
           "app-scoped refresh must invoke only Claude");
    assert(app_registry_request_refresh(&apps, APP_ID_CODEX));
    assert(claude_state.refresh_calls == 1 && codex_state.refresh_calls == 1 &&
           "app-scoped refresh must invoke only Codex");
    claude_state.enabled = false;
    assert(!app_registry_request_refresh(&apps, APP_ID_CLAUDE) &&
           claude_state.refresh_calls == 1 && "disabled app cannot refresh");
    assert(write_ppm(output) == 0);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: native-ui SCENE OUTPUT.ppm\n");
        return 2;
    }
    char five_reset[32], week_reset[32];
    if (strncmp(argv[1], "provider-", 9) == 0) {
        provider_scene(argv[1], argv[2]);
        return 0;
    }
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    if (strcmp(argv[1], "forecast-preclock") == 0) {
        forecast_preclock_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "boot") == 0) {
        boot_capture(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "boot-slow") == 0) {
        boot_slow_scene();
        return write_ppm(argv[2]);
    }
    if (strcmp(argv[1], "boot-interrupt") == 0) {
        boot_interrupt_scene();
        return write_ppm(argv[2]);
    }
    if (strcmp(argv[1], "clock-cycle") == 0) {
        clock_cycle_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "clock-attention") == 0) {
        clock_attention_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "header-actions") == 0) {
        header_actions_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "settings-flow") == 0) {
        settings_flow_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "wifi-flow") == 0) {
        wifi_flow_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "config-custom") == 0) {
        config_custom_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "shell-policy") == 0) {
        shell_policy_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "shell-ui") == 0) {
        shell_ui_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "shell-attention") == 0) {
        shell_attention_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "shell-guidance") == 0) {
        shell_guidance_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "shell-codex-weekly") == 0) {
        shell_codex_weekly_scene(argv[2]);
        return 0;
    }
    if (strncmp(argv[1], "clock-", 6) == 0) {
        clock_scene(argv[1], argv[2]);
        return 0;
    }
    display_init();
    display_set_device_name("hello-box-4827");
    display_clock_ready();
    settle_boot();
    if (strcmp(argv[1], "mascot-motion") == 0) {
        mascot_motion_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "footer-clean") == 0) {
        footer_clean_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "footer-minute") == 0) {
        footer_minute_scene(argv[2]);
        return 0;
    }
    if (strncmp(argv[1], "mascot-pause-", 13) == 0) {
        mascot_pause_scene(argv[1] + 13, argv[2]);
        return write_ppm(argv[2]);
    }
    if (strcmp(argv[1], "forecast") == 0) {
        forecast_scene(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "usage") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        open_claude_detail();
    } else if (strcmp(argv[1], "usage-full") == 0) {
        display_usage(100.0f, five_reset, 100.0f, week_reset);
        open_claude_detail();
        lv_refr_now(NULL);
        assert(frame[166 * WIDTH + 50].full == lv_color_hex(0x000000).full &&
               frame[316 * WIDTH + 50].full == lv_color_hex(0x000000).full &&
               "zero remaining quota must not invent a colored fill");
        display_usage(0.0f, five_reset, 0.0f, week_reset);
    } else if (strcmp(argv[1], "unknown") == 0) {
        display_usage(NAN, NULL, NAN, NULL);
        open_claude_detail();
        assert(contains_label(lv_scr_act(), "Unknown"));
        assert(!contains_label(lv_scr_act(), "* "));
        assert_age_label("Just now");
    } else if (strcmp(argv[1], "offline") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        open_claude_detail();
        fake_us += 901LL * 1000000LL;
        lv_tick_inc(1000);
        lv_timer_handler();
        open_claude_detail();
        assert(contains_label(lv_scr_act(), "Stale"));
        assert_age_label("15 minutes ago");
    } else if (strcmp(argv[1], "setup") == 0) {
        display_setup("http://192.168.4.1/setup");
        assert(contains_label(lv_scr_act(), "hello-box-4827"));
        display_set_device_name("hello-box-9371");
        assert(contains_label(lv_scr_act(), "hello-box-9371"));
        display_set_device_name("hello-box-4827");
    } else if (strcmp(argv[1], "updating") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        open_claude_detail();
        display_updating();
        assert(contains_label(lv_scr_act(), "Updating..."));
        assert(!contains_label(lv_scr_act(), "* "));
        assert_age_label("Just now");
        assert(contains_label(lv_scr_act(), "50%"));
    } else if (strcmp(argv[1], "face-refresh") == 0) {
        face_refresh(argv[2], five_reset, week_reset);
        return 0;
    } else if (strcmp(argv[1], "error") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        open_claude_detail();
        display_updating();
        display_status("Usage", "Connection failed; check Wi-Fi settings and try again later");
        assert(contains_label(lv_scr_act(), "Error:"));
        assert_usage_footer();
        assert(!contains_label(lv_scr_act(), "Updating..."));
        assert(contains_label(lv_scr_act(), "50%"));
        display_status("Time", "Waiting for clock");
        assert(contains_label(lv_scr_act(), "Waiting:"));
        assert(!contains_label(lv_scr_act(), "Error:"));
        assert(contains_label(lv_scr_act(), "50%"));
        display_status("Usage", "Retrying after cooldown");
        assert(contains_label(lv_scr_act(), "Retry:"));
        assert(!contains_label(lv_scr_act(), "Error:"));
        assert(contains_label(lv_scr_act(), "50%"));
        display_status("Usage", "Usage check failed; retrying");
        assert(contains_label(lv_scr_act(), "Error:"));
    } else if (strcmp(argv[1], "footer-error") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        open_claude_detail();
        step_ms(120000);
        display_status("Usage", "Connection failed; check Wi-Fi settings and try again later");
        assert(contains_label(lv_scr_act(), "Error:"));
        assert(!contains_label(lv_scr_act(), "* "));
        assert_age_label("2 minutes ago");
    } else if (strcmp(argv[1], "disconnected") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        open_claude_detail();
        display_status("Wi-Fi", "Connection failed; retrying");
        assert(contains_label(lv_scr_act(), "Offline:"));
        assert(!contains_label(lv_scr_act(), "* "));
        assert_age_label("Just now");
        assert(contains_label(lv_scr_act(), "50%"));
    } else {
        fprintf(stderr, "unknown scene: %s\n", argv[1]);
        return 2;
    }
    lv_refr_now(NULL);
    assert_black_corners();
    if (strcmp(argv[1], "offline") == 0) assert_stale_fill_pixels();
    if (strcmp(argv[1], "unknown") == 0)
        assert(frame[166 * WIDTH + 100].full == lv_color_hex(0x000000).full &&
               frame[316 * WIDTH + 60].full == lv_color_hex(0x000000).full &&
               "unknown quota must not invent a colored fill");
    if (strcmp(argv[1], "setup") != 0)
        assert_flat_dashboard_pixels(strcmp(argv[1], "usage") == 0);
    return write_ppm(argv[2]);
}
