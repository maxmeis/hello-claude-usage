#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <assert.h>

#include "display.h"
#include "display_format.h"
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
static int64_t fake_us = 1000000;
static uint32_t scenario_elapsed_ms;

static int write_ppm(const char *path);
static int contains_label(lv_obj_t *object, const char *prefix);
static lv_obj_t *find_label(lv_obj_t *object, const char *text);
static void assert_usage_footer(void);
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
    static const time_t test_epoch = 1761500000;
    if (out) *out = test_epoch;
    return test_epoch;
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

static void assert_usage_card_pixels(int assert_quota_palette)
{
    const lv_color_t card = lv_color_hex(0x252B27);
    /* Flat card interior, away from text, pill, progress track, and rounded edges. */
    assert(frame[160 * WIDTH + 400].full == card.full && "first card center must use the flat card fill");
    assert(frame[310 * WIDTH + 400].full == card.full && "second card center must use the flat card fill");

    /* Card edges stay inside their intended 432x132 bounds with no inherited outline. */
    assert(frame[156 * WIDTH + 24].full == card.full);
    assert(frame[156 * WIDTH + 23].full == lv_color_hex(0x000000).full);
    assert(frame[306 * WIDTH + 24].full == card.full);
    assert(frame[306 * WIDTH + 23].full == lv_color_hex(0x000000).full);
    assert(frame[238 * WIDTH + 400].full == lv_color_hex(0x000000).full);
    assert(frame[388 * WIDTH + 400].full == lv_color_hex(0x000000).full);

    /* Check the visible progress color after LVGL's RGB565 palette conversion. */
    if (assert_quota_palette) {
        lv_color_t quota = lv_color_hex(display_quota_color(50.0f));
        assert(quota.full == frame[179 * WIDTH + 100].full &&
               "quota bar must render its palette color through RGB565");
        assert(frame[179 * WIDTH + 250].full == lv_color_hex(0x655A64).full &&
               "remaining quota track must retain its RGB565 palette color");
    }
}

static void assert_stale_fill_pixels(void)
{
    const lv_color_t track = lv_color_hex(0x655A64);
    const int samples[][2] = {{100, 179}, {60, 329}};
    const float remaining[] = {50.0f, 11.0f};
    for (size_t i = 0; i < 2; ++i) {
        lv_color_t quota = lv_color_hex(display_quota_color(remaining[i]));
        lv_color_t faded = lv_color_mix(quota, track, LV_OPA_50);
        lv_color_t actual = frame[samples[i][1] * WIDTH + samples[i][0]];
        lv_color32_t actual_rgb, faded_rgb;
        actual_rgb.full = lv_color_to32(actual);
        faded_rgb.full = lv_color_to32(faded);
        int red_delta = (int)actual_rgb.ch.red - (int)faded_rgb.ch.red;
        int green_delta = (int)actual_rgb.ch.green - (int)faded_rgb.ch.green;
        int blue_delta = (int)actual_rgb.ch.blue - (int)faded_rgb.ch.blue;
        assert(abs(red_delta) <= 12 && abs(green_delta) <= 12 && abs(blue_delta) <= 12 &&
               "stale progress fill must remain visible at half opacity over its track");
        assert(actual.full != track.full &&
               "stale progress fill must not disappear into the track");
    }
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
    assert_usage_card_pixels(1);
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
    pointer_tap(24, 405);
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
    assert(contains_label(lv_scr_act(), "Updating:"));
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
    assert(contains_label(lv_scr_act(), "Updating:"));
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
    assert(contains_label(lv_scr_act(), "Updating:"));
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

static void assert_mascot_only_changed(const lv_color_t *reference)
{
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            if ((x >= 24 && x < 86 && y >= 24 && y < 70) || y >= 405) continue;
            assert(frame[y * WIDTH + x].full == reference[y * WIDTH + x].full &&
                   "mascot movement must not move or repaint other dashboard content");
        }
    }
}

static void assert_mascot_region_equal(const lv_color_t *reference)
{
    for (int y = 24; y < 70; ++y)
        for (int x = 24; x < 86; ++x)
            assert(frame[y * WIDTH + x].full == reference[y * WIDTH + x].full &&
                   "mascot pixels must remain still");
}

static void mascot_orange_bounds(int *min_x, int *min_y, int *max_x, int *max_y)
{
    *min_x = WIDTH;
    *min_y = HEIGHT;
    *max_x = -1;
    *max_y = -1;
    for (int y = 20; y < 76; ++y) {
        for (int x = 20; x < 92; ++x) {
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
    (void)max_x;
    (void)max_y;
    assert(min_x >= 28 && min_x <= 32 && "mascot horizontal shuffle must stay within four pixels");
    assert(min_y >= 28 && min_y <= 30 && "mascot bob must stay within two pixels");
}

static void mascot_motion_scene(const char *base)
{
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    scenario_elapsed_ms = 0;
    lv_refr_now(NULL);
    lv_color_t initial[WIDTH * HEIGHT];
    memcpy(initial, frame, sizeof(initial));
    step_until(6000);
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
    bool bob_seen = false;
    for (uint32_t t = 6020; t <= 8200; t += 20) {
        step_until(t);
        lv_refr_now(NULL);
        int changed = 0;
        for (int y = 24; y < 70; ++y)
            for (int x = 24; x < 86; ++x)
                changed += frame[y * WIDTH + x].full != rest[y * WIDTH + x].full;
        if (changed == 0) continue;
        ++moving_samples;
        assert_mascot_only_changed(rest);
        assert_mascot_offset_is_bounded();
        int min_x, min_y, max_x, max_y;
        mascot_orange_bounds(&min_x, &min_y, &max_x, &max_y);
        assert(min_x >= 28 && min_x <= 32 && min_y >= 28 && min_y <= 30 &&
               max_x <= 75 && max_y <= 62 &&
               "shuffle positions must stay within four horizontal and two vertical pixels");
        if (min_y > 28 && !bob_seen) {
            bob_seen = true;
            save_named_frame(base, "bob");
        }
        int pose_changed = 0;
        for (int y = 24; y < 70; ++y)
            for (int x = 24; x < 86; ++x)
                pose_changed |= frame[y * WIDTH + x].full != previous[y * WIDTH + x].full;
        if (pose_changed && captured < 10) {
            char name[32];
            snprintf(name, sizeof(name), "shuffle-%02d", ++captured);
            save_named_frame(base, name);
            memcpy(previous, frame, sizeof(previous));
        }
    }
    assert(moving_samples > 0 && captured > 0 && "idle mascot must visibly shuffle");
    assert(bob_seen && "shuffle must include a visible vertical bob");
    step_until(8400);
    lv_refr_now(NULL);
    assert_mascot_region_equal(rest);
    save_named_frame(base, "return");

    step_until(16000);
    lv_refr_now(NULL);
    assert_mascot_region_equal(rest);
    save_named_frame(base, "return-two");
}

static void mascot_pause_scene(const char *scene, const char *base)
{
    display_usage(50.0f, "2026-10-26T23:00:00Z", 89.0f, "2026-11-02T20:00:00Z");
    scenario_elapsed_ms = 0;
    lv_refr_now(NULL);
    lv_color_t initial[WIDTH * HEIGHT];
    memcpy(initial, frame, sizeof(initial));
    bool moving = false;
    for (uint32_t t = 6020; t <= 6800 && !moving; t += 20) {
        step_until(t);
        lv_refr_now(NULL);
        for (int y = 24; y < 70 && !moving; ++y)
            for (int x = 24; x < 86; ++x)
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
        for (int y = 24; y < 70; ++y)
            for (int x = 24; x < 86; ++x)
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
        for (int y = 24; y < 70; ++y)
            for (int x = 24; x < 86; ++x)
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
    assert(lv_obj_get_y(footer) == 409 && lv_obj_get_height(footer) == 51 &&
           "long error must wrap inside the unchanged footer bounds");
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
    assert_usage_card_pixels(1);
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
    assert_usage_card_pixels(1);
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

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: native-ui SCENE OUTPUT.ppm\n");
        return 2;
    }
    char five_reset[32], week_reset[32];
    reset_iso(five_reset, sizeof(five_reset), 3 * 3600);
    reset_iso(week_reset, sizeof(week_reset), 7 * 86400);
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
    display_init();
    display_clock_ready();
    settle_boot();
    if (strcmp(argv[1], "mascot-motion") == 0) {
        mascot_motion_scene(argv[2]);
        return 0;
    }
    if (strncmp(argv[1], "mascot-pause-", 13) == 0) {
        mascot_pause_scene(argv[1] + 13, argv[2]);
        return write_ppm(argv[2]);
    }
    if (strcmp(argv[1], "usage") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
    } else if (strcmp(argv[1], "usage-full") == 0) {
        display_usage(0.0f, five_reset, 0.0f, week_reset);
    } else if (strcmp(argv[1], "unknown") == 0) {
        display_usage(NAN, NULL, NAN, NULL);
    } else if (strcmp(argv[1], "offline") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        fake_us += 901LL * 1000000LL;
        lv_tick_inc(1000);
        lv_timer_handler();
    } else if (strcmp(argv[1], "setup") == 0) {
        display_setup("http://192.168.4.1/setup");
    } else if (strcmp(argv[1], "updating") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        display_updating();
        assert(contains_label(lv_scr_act(), "Updating:"));
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
        assert(!contains_label(lv_scr_act(), "Updating:"));
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
    } else if (strcmp(argv[1], "disconnected") == 0) {
        display_usage(50.0f, five_reset, 89.0f, week_reset);
        display_status("Wi-Fi", "Connection failed; retrying");
        assert(contains_label(lv_scr_act(), "Offline:"));
        assert(contains_label(lv_scr_act(), "50%"));
    } else {
        fprintf(stderr, "unknown scene: %s\n", argv[1]);
        return 2;
    }
    lv_refr_now(NULL);
    assert_black_corners();
    if (strcmp(argv[1], "offline") == 0) assert_stale_fill_pixels();
    if (strcmp(argv[1], "setup") != 0)
        assert_usage_card_pixels(strcmp(argv[1], "usage") == 0);
    return write_ppm(argv[2]);
}
