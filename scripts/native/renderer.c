#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <assert.h>

#include "display.h"
#include "display_format.h"
#include "word_clock.h"
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
static bool settings_accept = true;
static bool settings_close_accept = true;
static unsigned settings_opens, settings_closes;
static uint32_t last_settings_id, last_closed_id;
static int64_t fake_us = 1000000;
static uint32_t scenario_elapsed_ms;
static unsigned board_assert_count;
static time_t mock_epoch = 1761500000;

static int write_ppm(const char *path);
static int contains_label(lv_obj_t *object, const char *prefix);
static lv_obj_t *find_label(lv_obj_t *object, const char *text);
static void assert_usage_footer(void);
static void assert_age_label(const char *text);
static void assert_clock_view(void);
static lv_obj_t *find_clock(lv_obj_t *object);
static void assert_clock_unchanged(void);
static lv_obj_t *clock_label;
static char clock_text[8];
static lv_area_t clock_bounds;

static void mascot_motion_scene(const char *base);
static void mascot_pause_scene(const char *scene, const char *base);

int64_t esp_timer_get_time(void) { return fake_us; }
time_t time(time_t *out)
{
    if (out) *out = mock_epoch;
    return mock_epoch;
}
void lv_port_sem_take(void) {}
void lv_port_sem_give(void) {}

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

static void assert_flat_dashboard_pixels(int assert_quota_palette)
{
    const lv_color_t black = lv_color_hex(0x000000);
    const lv_color_t reset_ink = lv_color_hex(0xDDDCD6);
    for (int row = 0; row < 2; ++row) {
        int y = 206 + row * 150;
        assert(frame[(y + 8) * WIDTH + 48].full == reset_ink.full &&
               "reset clock hands must render next to each countdown");
        assert(frame[y * WIDTH + 40].full == black.full &&
               frame[(y + 15) * WIDTH + 55].full == black.full &&
               "reset clock must have a round outline on black");
    }
    /* Former card interiors, rounded edges, and both period-pill rectangles are transparent. */
    const int backgrounds[][2] = {
        {450, 160}, {450, 310}, {24, 156}, {24, 306}, {23, 156}, {23, 306},
        {400, 238}, {400, 388}, {320, 160}, {450, 160}, {320, 310}, {450, 310},
    };
    for (size_t i = 0; i < sizeof(backgrounds) / sizeof(backgrounds[0]); ++i) {
        if (frame[backgrounds[i][1] * WIDTH + backgrounds[i][0]].full != black.full)
            fprintf(stderr, "flat dashboard pixel (%d,%d) is 0x%04x, expected black\n",
                    backgrounds[i][0], backgrounds[i][1],
                    (unsigned)frame[backgrounds[i][1] * WIDTH + backgrounds[i][0]].full);
        assert(frame[backgrounds[i][1] * WIDTH + backgrounds[i][0]].full == black.full &&
               "dashboard row, old card, and old pill backgrounds must be pure black");
    }

    /* Check the visible progress color after LVGL's RGB565 palette conversion. */
    if (assert_quota_palette) {
        lv_color_t quota = lv_color_hex(display_quota_color(50.0f));
        assert(quota.full == frame[190 * WIDTH + 100].full &&
               "24-pixel quota bar must render its palette color through RGB565");
        assert(frame[190 * WIDTH + 250].full == black.full &&
               frame[340 * WIDTH + 100].full == black.full &&
               "unfilled quota area must show the black screen directly");
        assert(frame[196 * WIDTH + 100].full == black.full &&
               frame[346 * WIDTH + 60].full == black.full &&
               "24-pixel fills must stop above reset labels");
    }
}

static void assert_stale_fill_pixels(void)
{
    const lv_color_t black = lv_color_hex(0x000000);
    const int samples[][2] = {{100, 190}, {60, 340}};
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
    assert(frame[190 * WIDTH + 250].full == black.full &&
           frame[340 * WIDTH + 100].full == black.full);
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
    assert_clock_unchanged();
}

static void assert_clock_unchanged(void)
{
    assert(clock_label);
    assert(lv_obj_get_x(clock_label) == clock_bounds.x1);
    assert(lv_obj_get_y(clock_label) == clock_bounds.y1);
    assert(lv_obj_get_width(clock_label) == clock_bounds.x2 - clock_bounds.x1 + 1);
    assert(lv_obj_get_height(clock_label) == clock_bounds.y2 - clock_bounds.y1 + 1);
    assert(strcmp(lv_label_get_text(clock_label), clock_text) == 0);
}

static void assert_face_closed(void)
{
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_face_pixel(160, 190, 0xD97757);
    assert_face_pixel(320, 190, 0xD97757);
    assert_clock_unchanged();
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
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    lv_refr_now(NULL);
    assert_black_corners();
    assert_flat_dashboard_pixels(1);
    display_set_refresh_callback(mock_refresh, &refresh_requests);
    lv_refr_now(NULL);
    clock_label = find_clock(lv_scr_act());
    assert(clock_label);
    snprintf(clock_text, sizeof(clock_text), "%s", lv_label_get_text(clock_label));
    clock_bounds.x1 = lv_obj_get_x(clock_label);
    clock_bounds.y1 = lv_obj_get_y(clock_label);
    clock_bounds.x2 = clock_bounds.x1 + lv_obj_get_width(clock_label) - 1;
    clock_bounds.y2 = clock_bounds.y1 + lv_obj_get_height(clock_label) - 1;
    assert_clock_unchanged();

    /* Actual LVGL pointer events target the footer hitbox, including its edges. */
    refresh_accept = false;
    pointer_tap(112, 405);
    assert(refresh_requests == 1 && "footer hitbox includes its top-left bound");
    refresh_accept = true;
    pointer_tap(455, 455);
    assert(refresh_requests == 2 && "rejected callback must leave tap retryable");
    pointer_tap(240, 430);
    assert(refresh_requests == 2 && "accepted tap remains pending until cycle begins");
    display_refresh_cycle_begin();
    scenario_elapsed_ms = 0;
    assert(refresh_requests == 2);
    assert(!pixel_is_orange(10, 10) && "face starts transparent at cycle begin");
    display_updating();
    step_until(320);
    lv_refr_now(NULL);
    assert_face_open();
    assert(contains_label(lv_scr_act(), "Updating..."));
    pointer_tap(240, 430);
    assert(refresh_requests == 2 && "footer tap is ignored while face is active");
    char path[512];
    make_frame_path(path, sizeof(path), base, "open");
    save_ppm(path);

    step_until(1560);
    step_until(1700);
    lv_refr_now(NULL);
    assert_face_closed();
    make_frame_path(path, sizeof(path), base, "both-closed");
    save_ppm(path);

    step_until(1840);
    lv_refr_now(NULL);
    assert_face_open();
    step_until(2860);
    step_until(3000);
    lv_refr_now(NULL);
    assert_face_pixel(160, 190, 0xD97757);
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 190, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_clock_unchanged();
    make_frame_path(path, sizeof(path), base, "wink");
    save_ppm(path);

    step_until(3180);
    step_until(4600);
    lv_refr_now(NULL);
    assert_face_open();
    step_until(5000);
    lv_refr_now(NULL);
    assert(!pixel_is_orange(10, 10) && "face overlay is hidden after five seconds");
    assert(contains_label(lv_scr_act(), "Updating..."));
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
    const int x0 = 288, y0 = row == 0 ? 130 : 280;
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
    char five[32], week[32];
    display_usage(10.0f, NULL, 5.0f, NULL);
    reset_iso(five, sizeof(five), 18000 - 7200);
    reset_iso(week, sizeof(week), 604800 - 2 * 86400);
    display_usage(10.0f, five, 5.0f, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_HEADROOM);
    assert_percent_glyph(0, 2);
    assert_percent_glyph(1, 2);
    assert_flat_dashboard_pixels(0);
    save_named_frame(base, "headroom");

    display_usage(50.0f, five, 30.0f, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_TIGHT);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    assert_percent_glyph(0, 2);
    assert_percent_glyph(1, 2);
    save_named_frame(base, "tight");
    display_usage(10.0f, five, NAN, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "unknown-week");
    display_usage(NAN, five, 30.0f, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    save_named_frame(base, "unknown-five");
    display_usage(10.0f, five, 30.0f, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    save_named_frame(base, "mixed");

    mock_epoch += 10;
    step_ms(10000);
    assert_forecast_pixels(0, DISPLAY_FORECAST_HEADROOM);
    assert_forecast_pixels(1, DISPLAY_FORECAST_TIGHT);
    display_usage(50.0f, five, 5.0f, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_TIGHT);
    assert_forecast_pixels(1, DISPLAY_FORECAST_HEADROOM);

    display_usage(0.0f, five, 0.0f, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "zero");
    reset_iso(five, sizeof(five), 18000 - 60);
    reset_iso(week, sizeof(week), 604800 - 60);
    display_usage(10.0f, five, 10.0f, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "early");
    display_usage(10.0f, NULL, 10.0f, NULL);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "missing");

    reset_iso(five, sizeof(five), 18000 - 7200);
    reset_iso(week, sizeof(week), 604800 - 2 * 86400);
    display_usage(50.0f, five, 30.0f, week);
    fake_us += 901LL * 1000000LL;
    mock_epoch += 901;
    lv_tick_inc(1000);
    lv_timer_handler();
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "stale");
    display_usage(50.0f, five, 30.0f, week);
    display_status("Usage", "Connection failed");
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "error");
    reset_iso(five, sizeof(five), -1);
    reset_iso(week, sizeof(week), -1);
    display_usage(50.0f, five, 30.0f, week);
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    save_named_frame(base, "expired");
}

static void forecast_preclock_scene(const char *path)
{
    char five[32], week[32];
    reset_iso(five, sizeof(five), 18000 - 7200);
    reset_iso(week, sizeof(week), 604800 - 2 * 86400);
    display_init();
    display_usage(10.0f, five, 30.0f, week);
    display_clock_ready();
    fake_us += 5000LL * 1000LL;
    lv_tick_inc(5000);
    lv_timer_handler();
    assert_forecast_pixels(0, DISPLAY_FORECAST_UNKNOWN);
    assert_forecast_pixels(1, DISPLAY_FORECAST_UNKNOWN);
    display_usage(10.0f, five, 30.0f, week);
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
            if ((x >= 24 && x <= 104 && y >= 24 && y <= 80) || y >= 405) continue;
            assert(frame[y * WIDTH + x].full == reference[y * WIDTH + x].full &&
                   "mascot movement must not move or repaint other dashboard content");
        }
    }
}

static void assert_mascot_region_equal(const lv_color_t *reference)
{
    for (int y = 24; y <= 80; ++y)
        for (int x = 24; x <= 104; ++x)
            assert(frame[y * WIDTH + x].full == reference[y * WIDTH + x].full &&
                   "mascot pixels must remain still");
}

static void mascot_orange_bounds(int *min_x, int *min_y, int *max_x, int *max_y)
{
    *min_x = WIDTH;
    *min_y = HEIGHT;
    *max_x = -1;
    *max_y = -1;
    for (int y = 20; y <= 84; ++y) {
        for (int x = 20; x <= 108; ++x) {
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
    assert(min_x >= 24 && max_x <= 104 && min_y >= 24 && max_y <= 80 &&
           "all mascot pixels must remain in the clear header area");
}

static void mascot_motion_scene(const char *base)
{
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
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
    assert(rest_min_x == 28 && rest_min_y == 28 && rest_max_x == 71 && rest_max_y == 60 &&
           "resting mascot must preserve its original pixel geometry");
    assert(frame[40 * WIDTH + 42].full == lv_color_hex(0x000000).full &&
           frame[44 * WIDTH + 45].full == lv_color_hex(0x000000).full &&
           frame[40 * WIDTH + 54].full == lv_color_hex(0x000000).full &&
           frame[44 * WIDTH + 57].full == lv_color_hex(0x000000).full &&
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
        for (int y = 24; y <= 80; ++y)
            for (int x = 24; x <= 104; ++x)
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
        for (int y = 24; y <= 80; ++y)
            for (int x = 24; x <= 104; ++x)
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
    scenario_elapsed_ms = 0;
    lv_refr_now(NULL);
    lv_color_t initial[WIDTH * HEIGHT];
    memcpy(initial, frame, sizeof(initial));
    bool moving = false;
    for (uint32_t t = 1020; t <= 2200 && !moving; t += 20) {
        step_until(t);
        lv_refr_now(NULL);
        for (int y = 24; y <= 80 && !moving; ++y)
            for (int x = 24; x <= 104; ++x)
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
        pointer_tap(240, 430);
        assert(refresh_requests == 1 && "manual footer tap must still request a refresh");
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
        for (int y = 24; y <= 80; ++y)
            for (int x = 24; x <= 104; ++x)
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
        for (int y = 24; y <= 80; ++y)
            for (int x = 24; x <= 104; ++x)
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

static void footer_words_scene(const char *base)
{
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
    lv_refr_now(NULL);
    lv_obj_t *footer = find_label_prefix(lv_scr_act(), "* ");
    assert(footer && "healthy usage needs decorative center copy");
    assert(lv_obj_get_x(footer) == 24 && lv_obj_get_y(footer) == 438 &&
           lv_obj_get_width(footer) == 432 && lv_obj_get_height(footer) == 17);
    assert(lv_obj_get_style_text_font(footer, LV_PART_MAIN) == &usage_status_font);
    assert_age_label("Just now");
    lv_obj_t *settings = find_label(lv_scr_act(), "Settings");
    lv_obj_t *age = find_label(lv_scr_act(), "Just now");
    assert(settings && age);
    int baseline = lv_obj_get_y(footer) + usage_status_font.line_height - usage_status_font.base_line;
    assert(baseline == lv_obj_get_y(settings) + usage_meta_font.line_height - usage_meta_font.base_line &&
           baseline == lv_obj_get_y(age) + usage_meta_font.line_height - usage_meta_font.base_line &&
           "idle words, Settings and measurement age must share the same baseline");
    char first[64];
    snprintf(first, sizeof(first), "%s", lv_label_get_text(footer));
    lv_obj_t *clock = find_clock(lv_scr_act());
    assert(clock);
    char before_clock[8];
    snprintf(before_clock, sizeof(before_clock), "%s", lv_label_get_text(clock));
    save_named_frame(base, "start");
    step_ms(3100);
    assert(strncmp(lv_label_get_text(footer), "* ", 2) == 0 &&
           strcmp(lv_label_get_text(footer), first) != 0 &&
           "healthy idle words rotate after about three seconds");
    assert(strcmp(lv_label_get_text(clock), before_clock) == 0 &&
           "word rotation must not change the clock");
    assert_flat_dashboard_pixels(1);
    assert_age_label("Just now");
    save_named_frame(base, "next");
}

static void footer_minute_scene(const char *base)
{
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
    display_usage(50.0f, five_reset, 89.0f, week_reset);
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
    lv_obj_t *clock = find_clock(lv_scr_act());
    assert(clock && strcmp(lv_label_get_text(clock), "--:--") == 0 &&
           "clock stays unknown while SNTP is pending");
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
    const char *long_error = "Connection failed; check Wi-Fi settings and try again later";
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

    display_status("Usage", long_error);
    lv_refr_now(NULL);
    assert(contains_label(lv_scr_act(), "Error:"));
    assert_usage_footer();
    lv_obj_t *footer = find_label_prefix(lv_scr_act(), "Error:");
    assert(footer && strcmp(lv_label_get_text(footer),
                            "Error: Connection failed; check Wi-Fi settings and try again later") == 0 &&
           "long error detail must not be truncated while wrapping");
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

static lv_obj_t *find_clock(lv_obj_t *object)
{
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *text = lv_label_get_text(object);
        if (strlen(text) == 5 && text[2] == ':') return object;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i) {
        lv_obj_t *found = find_clock(lv_obj_get_child(object, i));
        if (found) return found;
    }
    return NULL;
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
            strcmp(lv_label_get_text(first), "E") == 0 &&
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
    lv_obj_t *mascot = find_clickable_rect(screen, 24, 24, 80, 56);
    lv_obj_t *clock = find_clickable_rect(screen, 350, 24, 106, 56);
    assert(mascot && clock && "fixed transparent header targets must exist at contract bounds");
    assert(lv_obj_get_style_bg_opa(mascot, LV_PART_MAIN) == LV_OPA_TRANSP &&
           lv_obj_get_style_bg_opa(clock, LV_PART_MAIN) == LV_OPA_TRANSP &&
           "header targets must not paint a background");
    assert(lv_obj_has_flag(mascot, LV_OBJ_FLAG_HIDDEN) == !visible &&
           lv_obj_has_flag(clock, LV_OBJ_FLAG_HIDDEN) == !visible &&
           "header targets must only be active on the dashboard");
}

static void assert_clock_mascot_hidden(void)
{
    lv_obj_t *mascot = header_mascot();
    assert(lv_obj_has_flag(mascot, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(mascot) == 24 && lv_obj_get_y(mascot) == 24 &&
           "hidden mascot must stay in its resting pose");
    assert(lv_obj_get_y(lv_obj_get_child(mascot, 3)) == 14 &&
           lv_obj_get_y(lv_obj_get_child(mascot, 4)) == 14 &&
           lv_obj_get_y(lv_obj_get_child(mascot, 5)) == 32 &&
           lv_obj_get_y(lv_obj_get_child(mascot, 7)) == 32 &&
           "hidden mascot limbs must stay at rest");
    for (int y = 24; y <= 80; ++y)
        for (int x = 28; x <= 104; ++x)
            assert(!pixel_is_orange(x, y) && "clock must not render mascot pixels");
}

static void assert_clock_view(void)
{
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
    if (frame[160 * WIDTH + 450].full != lv_color_hex(0x000000).full)
        fprintf(stderr, "dashboard assert #%u at scenario=%u ms: former card pixel is 0x%04x\n",
                board_assert_count, scenario_elapsed_ms,
                (unsigned)frame[160 * WIDTH + 450].full);
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
        display_init();
        example_usage();
        step_ms(5000);
        assert_board_view();
        assert_word_render(-1, -1, false);
        assert(contains_label(lv_scr_act(), "Waiting for time"));
        assert(strcmp(lv_label_get_text(find_clock(lv_scr_act())), "--:--") == 0);
        step_ms(60000);
        assert_board_view();
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
    assert(pixel_is_orange(40, 40) && "restored dashboard mascot must render");
    step_ms(1200);
    step_ms(1400);
    assert(lv_obj_get_x(header_mascot()) > 24 &&
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
    display_init();
    set_berlin_time(10, 8);
    display_clock_ready();
    settle_boot();
    clock_label = find_clock(lv_scr_act());
    assert(clock_label);
    snprintf(clock_text, sizeof(clock_text), "%s", lv_label_get_text(clock_label));
    clock_bounds.x1 = lv_obj_get_x(clock_label);
    clock_bounds.y1 = lv_obj_get_y(clock_label);
    clock_bounds.x2 = clock_bounds.x1 + lv_obj_get_width(clock_label) - 1;
    clock_bounds.y2 = clock_bounds.y1 + lv_obj_get_height(clock_label) - 1;
    display_set_refresh_callback(mock_refresh, &refresh_requests);
    assert_header_targets(true);
    assert_board_view();
    save_named_frame(base, "dashboard");

    /* Outside pointer press/release gestures must not activate either target. */
    pointer_tap(456, 79);
    pointer_tap(349, 24);
    pointer_gesture(456, 80, 457, 81);
    assert_board_view();
    assert(refresh_requests == 0);

    /* The fixed Clawd target remains usable while the mascot itself is walking. */
    step_ms(1200); /* Exercise the fixed hitbox after the mascot has moved. */
    scenario_elapsed_ms = 0;
    pointer_tap(104, 80); /* First pixel outside the fixed target. */
    assert_board_view();
    assert(refresh_requests == 0);
    pointer_tap(103, 79); /* Last inside pixel on both press and release. */
    scenario_elapsed_ms = 0; /* Face timing starts at the accepted pointer click. */
    assert(refresh_requests == 0 && "mascot action must never call the refresh callback");
    assert_age_label("Just now");
    step_until(320);
    lv_refr_now(NULL);
    assert(pixel_is_orange(10, 10) && "fixed mascot target must start the one-shot face");
    assert_face_open();
    assert_header_targets(true);
    save_named_frame(base, "mascot-open");

    pointer_tap(24, 24); /* A second mascot tap during the face must not restart it. */
    step_until(1560);
    step_until(1840);
    step_until(3000);
    lv_refr_now(NULL);
    assert_face_pixel(160, 190, 0xD97757);
    assert_face_pixel(160, 250, 0x151A17);
    assert_face_pixel(320, 190, 0x151A17);
    assert_face_pixel(320, 250, 0x151A17);
    assert_clock_unchanged();
    save_named_frame(base, "mascot-wink");

    /* The clock's final inside pixel works immediately, including during a face. */
    pointer_tap(455, 79);
    assert_clock_view();
    assert_header_targets(false);
    assert(refresh_requests == 0 && "clock header tap must never call the refresh callback");
    save_named_frame(base, "manual-clock");
    step_ms(1000);
    assert_clock_view();

    /* A new request cycle clears manual-clock mode; its response cannot bounce the view. */
    display_refresh_cycle_begin();
    display_updating();
    step_ms(320);
    assert(pixel_is_orange(10, 10) && "the next logical cycle restores the attention face");
    pointer_tap(350, 24);
    assert_clock_view();
    assert(refresh_requests == 0);
    save_named_frame(base, "inflight-clock");
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    display_refresh_cycle_end();
    assert_clock_view();
    step_ms(1000); /* refresh() ticks while the successful response is cached. */
    assert_clock_view();

    /* The word-clock surface returns to cached usage even with another request in flight. */
    display_refresh_cycle_begin();
    display_updating();
    step_ms(320);
    pointer_tap(350, 24);
    assert_clock_view();
    pointer_tap(240, 240);
    assert_board_view();
    assert(refresh_requests == 0);
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    display_refresh_cycle_end();
    assert_board_view();

    /* A mascot gesture is inert on error and unknown data, then timestamp age keeps advancing. */
    display_status("Usage", "Connection failed; retry later");
    pointer_tap(24, 24);
    pointer_tap(350, 24);
    assert(!pixel_is_orange(10, 10) && "error guards both header actions");
    assert_board_view();
    assert(refresh_requests == 0);
    display_usage(NAN, NULL, NAN, NULL); /* Existing error recovery starts its normal face. */
    step_ms(5000);
    assert(!pixel_is_orange(10, 10) && "unknown-data recovery face must finish before guard taps");
    pointer_tap(24, 24);
    pointer_tap(350, 24);
    assert(!pixel_is_orange(10, 10) && "unknown data guards both header actions");
    assert_board_view();
    step_ms(60000);
    assert_age_label("1 minute ago");
    save_named_frame(base, "return-dashboard");

    /* Cached data that is already stale cannot enter manual clock mode. */
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    step_ms(901000);
    assert_board_view();
    assert(find_label_prefix(lv_scr_act(), "Stale"));
    assert_stale_fill_pixels();
    pointer_tap(350, 24);
    assert_board_view();
    assert(find_label_prefix(lv_scr_act(), "Stale") && "stale clock tap must preserve attention");
    save_named_frame(base, "stale-dashboard");

    /* Fresh cached data may enter manual clock during a request, until it itself becomes stale. */
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    display_refresh_cycle_begin();
    display_updating();
    step_ms(320);
    assert(pixel_is_orange(10, 10));
    pointer_tap(350, 24);
    assert_clock_view();
    step_ms(901000);
    assert_board_view();
    assert(find_label_prefix(lv_scr_act(), "Stale") &&
           "manual clock must give stale attention priority during in-flight refresh");
    assert_stale_fill_pixels();
    save_named_frame(base, "stale-inflight");
    display_refresh_cycle_end();
    assert_board_view();

    /* Setup owns the screen above header targets and suppresses both actions. */
    display_setup("http://192.168.4.1/setup");
    assert_header_targets(false);
    pointer_tap(24, 24);
    pointer_tap(350, 24);
    assert(contains_label(lv_scr_act(), "Connect Claude"));
    assert(refresh_requests == 0);
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

static void assert_overlay_qr(lv_obj_t *overlay, bool shown)
{
    lv_obj_t *field = lv_obj_get_child(overlay, 1);
    lv_obj_t *qr = lv_obj_get_child(field, 0);
    if (lv_obj_has_flag(field, LV_OBJ_FLAG_HIDDEN) != !shown) {
        fprintf(stderr, "QR visibility mismatch: expected shown=%d, field hidden=%d, detail=%s\n",
                shown, lv_obj_has_flag(field, LV_OBJ_FLAG_HIDDEN),
                lv_label_get_text(lv_obj_get_child(overlay, 2)));
        abort();
    }
    assert(lv_obj_get_x(field) == 114 && lv_obj_get_y(field) == 99);
    assert(lv_obj_get_width(field) == 252 && lv_obj_get_height(field) == 252);
    assert(lv_obj_get_x(qr) == 30 && lv_obj_get_y(qr) == 30);
    assert(lv_obj_get_width(qr) == 192 && lv_obj_get_height(qr) == 192);
    lv_refr_now(NULL);
    if (shown) {
        const lv_color_t white = lv_color_hex(0xFFFFFF);
        assert(frame[105 * WIDTH + 120].full == white.full &&
               frame[345 * WIDTH + 360].full == white.full &&
               "QR quiet zone must remain white on both sides");
        int dark = 0, light = 0;
        for (int y = 135; y < 315; ++y)
            for (int x = 150; x < 330; ++x) {
                dark += frame[y * WIDTH + x].full == lv_color_hex(0x000000).full;
                light += frame[y * WIDTH + x].full == white.full;
            }
        assert(dark > 1000 && light > 1000 && "QR must contain visible modules");
    }
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
    lv_obj_t *settings_label = find_label(lv_scr_act(), "Settings");
    assert(settings_label && lv_obj_get_x(settings_label) == 24 &&
           lv_obj_get_y(settings_label) == 442);
    pointer_tap(23, 440);
    assert(settings_opens == 0 && refresh_requests == 0);
    pointer_tap(112, 416);
    assert(refresh_requests == 1 && settings_opens == 0 &&
           "refresh begins exactly after the Settings hitbox");
    display_refresh_cycle_begin();
    display_refresh_cycle_end();
    step_ms(5000);
    pointer_tap(24, 416);
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
    pointer_tap(24, 24);
    pointer_tap(350, 24);
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
    pointer_tap(24, 416);
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
    pointer_tap(24, 416);
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
    pointer_tap(24, 416);
    assert(settings_opens == 1);
    display_wifi_setup(join_qr,
                       "http://192.168.4.1/setup", false, NULL);
    assert(settings_closes == 0 &&
           "Wi-Fi takes visual priority without sending a second CLOSE callback");
    lv_obj_t *settings = overlay_for_title("Settings");
    lv_obj_t *wifi = overlay_for_title("Join setup Wi-Fi");
    assert(lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN));
    assert(find_label(wifi, "hello-box-4827"));
    assert_overlay_qr(wifi, true);
    pointer_tap(24, 24);
    pointer_tap(350, 24);
    assert(!lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN) && settings_opens == 1 &&
           refresh_requests == 0 && "Wi-Fi overlay must guard both header actions");
    assert_device_name_pixels();
    display_set_device_name("hello-box-9371");
    assert(find_label(wifi, "hello-box-9371"));
    assert_overlay_qr(wifi, true);
    display_set_device_name("hello-box-4827");
    assert(find_label(wifi, "hello-box-4827"));
    assert_overlay_qr(wifi, true);
    assert_device_name_pixels();
    save_named_frame(base, "join");
    display_status("Wi-Fi", "Connection failed; retrying");
    display_updating();
    display_refresh_cycle_begin();
    step_ms(6000);
    assert(!lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN));
    assert_overlay_qr(wifi, true);
    display_wifi_setup(join_qr,
                       "http://192.168.4.1/setup", true, NULL);
    assert(find_label(wifi, "Open Wi-Fi setup"));
    assert_overlay_qr(wifi, true);
    assert_device_name_pixels();
    save_named_frame(base, "page");
    display_usage(30.0f, "2026-10-26T23:00:00Z", 70.0f, "2026-11-02T20:00:00Z");
    display_refresh_cycle_end();
    assert(!lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN));
    display_wifi_setup_done();
    assert(lv_obj_has_flag(wifi, LV_OBJ_FLAG_HIDDEN));
    assert(contains_label(lv_scr_act(), "70%"));
    save_named_frame(base, "done");
    step_ms(29000);
    assert_board_view();
    step_ms(1000);
    assert_clock_view();
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: native-ui SCENE OUTPUT.ppm\n");
        return 2;
    }
    char five_reset[32], week_reset[32];
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
    if (strcmp(argv[1], "footer-words") == 0) {
        footer_words_scene(argv[2]);
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
    } else if (strcmp(argv[1], "usage-full") == 0) {
        display_usage(100.0f, five_reset, 100.0f, week_reset);
        lv_refr_now(NULL);
        assert(frame[190 * WIDTH + 100].full == lv_color_hex(0x000000).full &&
               frame[340 * WIDTH + 60].full == lv_color_hex(0x000000).full &&
               "zero remaining quota must not invent a colored fill");
        display_usage(0.0f, five_reset, 0.0f, week_reset);
    } else if (strcmp(argv[1], "unknown") == 0) {
        display_usage(NAN, NULL, NAN, NULL);
        assert(contains_label(lv_scr_act(), "Unknown"));
        assert(!contains_label(lv_scr_act(), "* "));
        assert_age_label("Just now");
    } else if (strcmp(argv[1], "offline") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        fake_us += 901LL * 1000000LL;
        lv_tick_inc(1000);
        lv_timer_handler();
        assert(contains_label(lv_scr_act(), "Stale"));
        assert(!contains_label(lv_scr_act(), "* "));
        assert_age_label("15 minutes ago");
    } else if (strcmp(argv[1], "setup") == 0) {
        display_setup("http://192.168.4.1/setup");
        assert(contains_label(lv_scr_act(), "hello-box-4827"));
        display_set_device_name("hello-box-9371");
        assert(contains_label(lv_scr_act(), "hello-box-9371"));
        display_set_device_name("hello-box-4827");
    } else if (strcmp(argv[1], "updating") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
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
        step_ms(120000);
        display_status("Usage", "Connection failed; check Wi-Fi settings and try again later");
        assert(contains_label(lv_scr_act(), "Error:"));
        assert(!contains_label(lv_scr_act(), "* "));
        assert_age_label("2 minutes ago");
    } else if (strcmp(argv[1], "disconnected") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
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
        assert(frame[190 * WIDTH + 100].full == lv_color_hex(0x000000).full &&
               frame[340 * WIDTH + 60].full == lv_color_hex(0x000000).full &&
               "unknown quota must not invent a colored fill");
    if (strcmp(argv[1], "setup") != 0)
        assert_flat_dashboard_pixels(strcmp(argv[1], "usage") == 0);
    return write_ppm(argv[2]);
}
