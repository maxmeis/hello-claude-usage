#include "box_config.h"

#include <string.h>

static box_settings_t s_current = {0, 0, 100, 0, 30, 300, true, true, 0};
static const char *const s_timezones[] = {
    "CET-1CEST,M3.5.0,M10.5.0/3", "UTC0", "GMT0BST,M3.5.0/1,M10.5.0",
    "EST5EDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0", "JST-9"
};

const box_settings_t *box_config_current(void) { return &s_current; }
const char *box_config_timezone(void) { return s_timezones[s_current.timezone < 6 ? s_current.timezone : 0]; }

void box_config_defaults(box_settings_t *settings)
{
    if (!settings) return;
    *settings = (box_settings_t){0, 0, 100, 0, 30, 300, true, true, 0};
}

bool box_config_validate_settings(const box_settings_t *s)
{
    return s && s->clock_language <= 1 && s->default_view <= 1 &&
           s->brightness >= 20 && s->brightness <= 100 && s->timezone <= 5 &&
           s->dashboard_timeout_sec >= 10 && s->dashboard_timeout_sec <= 120 &&
           s->refresh_interval_sec >= 60 && s->refresh_interval_sec <= 600 &&
           s->providers <= 2;
}

void box_config_set_current(const box_settings_t *settings)
{
    if (box_config_validate_settings(settings)) s_current = *settings;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return ~crc;
}

static bool valid_ssid(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 32) return false;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) { ++i; continue; }
        unsigned count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!count || i + count >= n) return false;
        for (unsigned j = 1; j <= count; ++j)
            if (((unsigned char)s[i + j] & 0xc0) != 0x80) return false;
        unsigned char next = (unsigned char)s[i + 1];
        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) ||
            (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) return false;
        i += count + 1;
    }
    return true;
}

static bool valid_password(const char *p)
{
    size_t n = strlen(p);
    if (n && n < 8) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (n == 64) {
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
        } else if (c < 32 || c > 126) return false;
    }
    return true;
}

bool box_config_decode(const uint8_t *frame, size_t frame_size, box_config_image_t *out)
{
    if (!frame || !out || frame_size != BOX_CONFIG_FRAME_SIZE ||
        memcmp(frame, "HBX1", 4) || le32(frame + 4) != BOX_CONFIG_BODY_SIZE ||
        le32(frame + 8) != crc32_ieee(frame + 12, BOX_CONFIG_BODY_SIZE)) return false;
    for (size_t i = 12 + BOX_CONFIG_BODY_SIZE; i < BOX_CONFIG_FRAME_SIZE; ++i)
        if (frame[i] != 0xff) return false;
    const uint8_t *p = frame + 12;
    box_config_image_t value = {0};
    value.settings.clock_language = p[0];
    value.settings.default_view = p[1];
    value.settings.brightness = p[2];
    value.settings.dashboard_timeout_sec = (uint16_t)p[3] | (uint16_t)p[4] << 8;
    value.settings.refresh_interval_sec = (uint16_t)p[5] | (uint16_t)p[6] << 8;
    value.settings.timezone = p[7];
    value.settings.providers = (p[8] >> 2) & 3u;
    if (p[8] & ~15u || p[9] > 1 || !box_config_validate_settings(&value.settings)) return false;
    value.settings.mascot_motion = (p[8] & 1u) != 0;
    value.settings.refresh_animation = (p[8] & 2u) != 0;
    value.wifi_present = p[9] != 0;
    const uint8_t *ssid = p + 10, *password = p + 43;
    if (!value.wifi_present) {
        for (size_t i = 10; i < BOX_CONFIG_BODY_SIZE; ++i) if (p[i]) return false;
    } else {
        if (!memchr(ssid, 0, 33) || !memchr(password, 0, 65)) return false;
        size_t ssid_len = (const uint8_t *)memchr(ssid, 0, 33) - ssid;
        size_t pass_len = (const uint8_t *)memchr(password, 0, 65) - password;
        for (size_t i = ssid_len + 1; i < 33; ++i) if (ssid[i]) return false;
        for (size_t i = pass_len + 1; i < 65; ++i) if (password[i]) return false;
        memcpy(value.ssid, ssid, 33);
        memcpy(value.password, password, 65);
        if (!valid_ssid(value.ssid) || !valid_password(value.password)) return false;
    }
    *out = value;
    return true;
}
