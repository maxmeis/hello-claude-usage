#include "display_diagnostic.h"

#include <stdbool.h>
#include <stddef.h>

#ifndef DISPLAY_DIAGNOSTIC_HOST
#include "bsp_board.h"
#include "bsp_i2c.h"
#include "bsp_lcd.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_log.h"
#endif

enum {
    BLACK = 0x0000,
    WHITE = 0xFFFF,
    RED = 0xF800,
    GREEN = 0x07E0,
    BLUE = 0x001F,
    CLAY = 0xDBAA,
    GRAY = 0x8410,
};

typedef struct {
    char character;
    uint8_t row[7];
} glyph_t;

/* Five pixels per row, most significant bit on the left. */
static const glyph_t glyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},
};

static void dot(uint16_t *pixels, int width, int height, int x, int y,
                uint16_t color)
{
    if (x >= 0 && x < width && y >= 0 && y < height) {
        pixels[(size_t)y * (size_t)width + (size_t)x] = color;
    }
}

static void rect(uint16_t *pixels, int width, int height,
                 int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            pixels[(size_t)y * (size_t)width + (size_t)x] = color;
        }
    }
}

static void label(uint16_t *pixels, int width, int height,
                  int x, int y, const char *text, uint16_t color)
{
    for (; *text; ++text, x += 12) {
        const glyph_t *glyph = NULL;
        for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
            if (glyphs[i].character == *text) {
                glyph = &glyphs[i];
                break;
            }
        }
        if (!glyph) continue;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (glyph->row[row] & (1u << (4 - col))) {
                    rect(pixels, width, height, x + col * 2, y + row * 2,
                         x + col * 2 + 2, y + row * 2 + 2, color);
                }
            }
        }
    }
}

void display_diagnostic_render(uint16_t *pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0) return;

    rect(pixels, width, height, 0, 0, width, height, BLACK);
    label(pixels, width, height, 8, 12, "RGB565 RAW 480X480", WHITE);

    static const uint16_t colors[7] = {
        BLACK, WHITE, RED, GREEN, BLUE, CLAY, GRAY,
    };
    static const uint16_t inks[7] = {
        WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, BLACK,
    };
    static const char *const names[7] = {
        "BLACK", "WHITE", "RED", "GREEN", "BLUE", "CLAY", "GRAY",
    };
    static const char *const codes[7] = {
        "0000", "FFFF", "F800", "07E0", "001F", "DBAA", "8410",
    };

    for (int i = 0; i < 7; ++i) {
        int x = 4 + (i % 4) * 120;
        int y = 48 + (i / 4) * 150;
        rect(pixels, width, height, x, y, x + 112, y + 142, colors[i]);
        label(pixels, width, height, x + 8, y + 8, names[i], inks[i]);
        label(pixels, width, height, x + 8, y + 28, codes[i], inks[i]);
    }

    /* The eighth block provides alternating whole pixels and 8-pixel cells. */
    rect(pixels, width, height, 364, 198, 476, 340, WHITE);
    label(pixels, width, height, 372, 206, "CHECK", BLACK);
    for (int y = 230; y < 320; ++y) {
        for (int x = 372; x < 468; ++x) {
            uint16_t color = y < 270
                ? (((x + y) & 1) ? BLACK : WHITE)
                : ((((x - 372) / 8 + (y - 270) / 8) & 1) ? BLACK : WHITE);
            dot(pixels, width, height, x, y, color);
        }
    }

    static const uint16_t ramp[8] = {
        0x0000, 0x2104, 0x4208, 0x630C,
        0x8410, 0xA514, 0xC618, 0xFFFF,
    };
    for (int i = 0; i < 8; ++i) {
        rect(pixels, width, height, i * 60, 354, (i + 1) * 60, 406, ramp[i]);
    }

    label(pixels, width, height, 8, 416, "1 PIXEL LINES", WHITE);
    for (int x = 4; x < 476; ++x) {
        dot(pixels, width, height, x, 432, WHITE);
        dot(pixels, width, height, x, 478, WHITE);
    }
    for (int y = 432; y < 479; ++y) {
        dot(pixels, width, height, 4, y, WHITE);
        dot(pixels, width, height, 475, y, WHITE);
    }
    for (int x = 12; x < 236; x += 4) {
        for (int y = 440; y < 472; ++y) {
            dot(pixels, width, height, x, y, WHITE);
        }
    }
    for (int y = 440; y < 472; ++y) {
        for (int x = 244; x < 468; ++x) {
            if ((x + y) & 1) dot(pixels, width, height, x, y, WHITE);
        }
    }
}

#ifndef DISPLAY_DIAGNOSTIC_HOST
static const char *const TAG = "display_check";

static bool flush_done(void *unused)
{
    (void)unused;
    return false;
}

static bool flush_is_last(void)
{
    return true;
}

void display_diagnostic_run(void)
{
    ESP_ERROR_CHECK(bsp_board_init());
    esp_err_t status_48 = bsp_i2c_probe_addr(0x48);
    esp_err_t status_38 = bsp_i2c_probe_addr(0x38);
    ESP_LOGI(TAG, "touch probe 0x48=%s (0x%x), 0x38=%s (0x%x)",
             esp_err_to_name(status_48), (unsigned)status_48,
             esp_err_to_name(status_38), (unsigned)status_38);

    const board_res_desc_t *board = bsp_board_get_description();
    ESP_ERROR_CHECK(board && board->LCD_WIDTH == DISPLAY_DIAGNOSTIC_WIDTH &&
                    board->LCD_HEIGHT == DISPLAY_DIAGNOSTIC_HEIGHT
                    ? ESP_OK : ESP_ERR_INVALID_SIZE);
    void *raw1 = NULL;
    void *raw2 = NULL;
    bsp_lcd_get_frame_buffer(&raw1, &raw2);
    ESP_ERROR_CHECK(raw1 && raw2 && raw1 != raw2 ? ESP_OK : ESP_ERR_INVALID_STATE);

    uint16_t *const frame1 = raw1;
    uint16_t *const frame2 = raw2;
    const size_t bytes = (size_t)DISPLAY_DIAGNOSTIC_WIDTH *
                         DISPLAY_DIAGNOSTIC_HEIGHT * sizeof(uint16_t);
    display_diagnostic_render(frame1, DISPLAY_DIAGNOSTIC_WIDTH,
                              DISPLAY_DIAGNOSTIC_HEIGHT);
    display_diagnostic_render(frame2, DISPLAY_DIAGNOSTIC_WIDTH,
                              DISPLAY_DIAGNOSTIC_HEIGHT);
    ESP_ERROR_CHECK(esp_cache_msync(frame1, bytes, 0));
    ESP_ERROR_CHECK(esp_cache_msync(frame2, bytes, 0));

    ESP_ERROR_CHECK(bsp_lcd_set_cb(flush_done, NULL));
    bsp_lcd_flush_is_last_register(flush_is_last);
    ESP_ERROR_CHECK(bsp_lcd_flush(0, 0, DISPLAY_DIAGNOSTIC_WIDTH,
                                  DISPLAY_DIAGNOSTIC_HEIGHT, frame1));
    ESP_LOGI(TAG, "raw RGB565 pattern submitted: 480x480, two synchronized framebuffers");
}
#endif
