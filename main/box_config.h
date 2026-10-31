#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOX_CONFIG_FRAME_SIZE 4096u
#define BOX_CONFIG_BODY_SIZE 108u

typedef struct {
    uint8_t clock_language;       /* 0 German, 1 English */
    uint8_t default_view;         /* 0 word-clock screensaver after idle, 1 Home always */
    uint8_t brightness;           /* 20..100 percent */
    uint8_t timezone;             /* 0 Berlin, 1 UTC, 2 London, 3 New York, 4 Los Angeles, 5 Tokyo */
    uint16_t dashboard_timeout_sec; /* Manual Home/app idle before screensaver. */
    uint16_t refresh_interval_sec;
    bool mascot_motion;
    bool refresh_animation;
    uint8_t providers;            /* 0 Claude, 1 Codex, 2 both */
} box_settings_t;

typedef struct {
    box_settings_t settings;
    bool wifi_present;
    char ssid[33];
    char password[65];
} box_config_image_t;

const box_settings_t *box_config_current(void);
const char *box_config_timezone(void);
void box_config_defaults(box_settings_t *settings);
bool box_config_validate_settings(const box_settings_t *settings);
bool box_config_decode(const uint8_t *frame, size_t frame_size, box_config_image_t *out);
void box_config_set_current(const box_settings_t *settings);
