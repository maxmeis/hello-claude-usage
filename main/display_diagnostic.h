#pragma once

#include <stdint.h>

/* Raw RGB565 diagnostic, designed for a 480 x 480 display. Coordinates are
 * half-open rectangles. Header: y=0..39. Two rows of four 112 x 142 blocks:
 * x=4,124,244,364; y=48,198. Row 1: BLACK 0x0000, WHITE 0xFFFF,
 * RED 0xF800, GREEN 0x07E0. Row 2: BLUE 0x001F, CLAY 0xDBAA
 * (#D97757 rounded down to RGB565), GRAY 0x8410, CHECKER 0x0000/0xFFFF.
 * Eight grayscale steps occupy y=354..405, then one-pixel alignment marks
 * and a fine checkerboard occupy y=416..479. Text uses raw 5 x 7 bitmap
 * glyphs, enlarged to 10 x 14 pixels by duplicating each pixel.
 * All color blocks have broad, untouched interiors for one-photo diagnosis.
 */
#define DISPLAY_DIAGNOSTIC_WIDTH 480
#define DISPLAY_DIAGNOSTIC_HEIGHT 480

/* The caller supplies at least width * height uint16_t pixels. No allocation,
 * LVGL, or hardware access occurs in this function. */
void display_diagnostic_render(uint16_t *pixels, int width, int height);

/* Initialize the BSP, probe the two touch addresses, fill and sync both RGB
 * framebuffers, and submit one full-frame flush. The caller may then return. */
void display_diagnostic_run(void);
