#include "usage_app_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display_format.h"
#include "usage_fonts.h"

#define BG 0x000000
#define TEXT 0xF9F6EF
#define MUTED 0xA6A49F
#define RULE 0x242B26
#define ORANGE 0xD97757
#define STALE_US (900LL * 1000000LL)

static lv_obj_t *caption(lv_obj_t *parent, const lv_font_t *font,
                         uint32_t color, int x, int y, int width,
                         const char *text)
{
    lv_obj_t *item = lv_label_create(parent);
    lv_obj_set_pos(item, x, y);
    lv_obj_set_width(item, width);
    lv_obj_set_style_text_font(item, font, 0);
    lv_obj_set_style_text_color(item, lv_color_hex(color), 0);
    lv_label_set_long_mode(item, LV_LABEL_LONG_DOT);
    lv_obj_update_layout(item);
    lv_label_set_text(item, text);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE);
    return item;
}

static lv_obj_t *bar(lv_obj_t *parent, int x, int y, int height)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_pos(item, x, y);
    lv_obj_set_size(item, 0, height);
    lv_obj_set_style_bg_color(item, lv_color_hex(ORANGE), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(item, 4, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE);
    return item;
}

void usage_app_widget_mount(usage_app_widget_t *widget, lv_obj_t *parent,
                            int x, int y, int width, int height,
                            usage_provider_id_t provider, const char *name)
{
    memset(widget, 0, sizeof(*widget));
    widget->weekly_only = provider == USAGE_PROVIDER_CODEX;
    widget->bar_width[0] = widget->weekly_only ? width - 32 : (width - 48) / 2;
    widget->bar_width[1] = (width - 48) / 2;
    widget->root = lv_obj_create(parent);
    lv_obj_remove_style_all(widget->root);
    lv_obj_set_pos(widget->root, x, y);
    lv_obj_set_size(widget->root, width, height);
    lv_obj_set_style_bg_color(widget->root, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(widget->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(widget->root, 0, 0);
    lv_obj_set_style_radius(widget->root, 0, 0);
    lv_obj_set_style_pad_all(widget->root, 0, 0);
    lv_obj_clear_flag(widget->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(widget->root, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *rule = lv_obj_create(widget->root);
    lv_obj_remove_style_all(rule);
    lv_obj_set_pos(rule, 16, height - 1);
    lv_obj_set_size(rule, width - 32, 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(RULE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);

    char setup[40];
    snprintf(setup, sizeof(setup), "Set up %s", name);
    widget->setup = caption(widget->root, &usage_status_font, TEXT,
                            16, height / 2 - 12, width - 32, setup);
    lv_obj_set_style_text_align(widget->setup, LV_TEXT_ALIGN_CENTER, 0);
    widget->title = caption(widget->root, &usage_ui_font, TEXT,
                            16, 14, 210, name);
    widget->state = caption(widget->root, &usage_meta_font, MUTED,
                            width - 220, 20, 204, "Waiting for usage");
    lv_obj_set_style_text_align(widget->state, LV_TEXT_ALIGN_RIGHT, 0);
    if (!widget->weekly_only) {
        lv_obj_t *divider = lv_obj_create(widget->root);
        widget->divider = divider;
        lv_obj_remove_style_all(divider);
        lv_obj_set_pos(divider, width / 2, 60);
        lv_obj_set_size(divider, 1, 108);
        lv_obj_set_style_bg_color(divider, lv_color_hex(RULE), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);
    }
    int count = widget->weekly_only ? 1 : 2;
    for (int i = 0; i < count; ++i) {
        int left = 16 + i * (widget->bar_width[i] + 16);
        int available = widget->bar_width[i];
        widget->window[i].name = caption(widget->root, &usage_status_font,
                                         MUTED, left, 52,
                                         available,
                                         widget->weekly_only ? "Week left" :
                                         i ? "Week left" : "5h left");
        widget->window[i].value = caption(widget->root,
                                          width <= 228 ? &usage_ui_font : &usage_values_font,
                                          TEXT, left, 70,
                                          available, "--%");
        widget->window[i].bar = bar(widget->root, left, 132,
                                    widget->weekly_only ? 14 : 12);
        widget->window[i].reset = caption(widget->root, &usage_status_font,
                                          MUTED, left, 153,
                                          available,
                                          "Reset unknown");
    }
    widget->tokens = caption(widget->root, &usage_meta_font, MUTED,
                             16, height - 27, width - 32,
                             "");
    lv_obj_set_style_text_align(widget->tokens, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(widget->tokens, LV_OBJ_FLAG_HIDDEN);
}

static void reset_text(char *out, size_t size, const quota_window_t *window,
                       bool clock_ready, int64_t now_epoch)
{
    if (!clock_ready || window->resets_at <= 0) {
        snprintf(out, size, "Reset unknown");
        return;
    }
    if (window->resets_at <= now_epoch) {
        snprintf(out, size, "Awaiting reset");
        return;
    }
    int64_t left = window->resets_at - now_epoch;
    if (left >= 86400)
        snprintf(out, size, "Resets in %lldd %lldh",
                 (long long)(left / 86400), (long long)((left % 86400) / 3600));
    else snprintf(out, size, "Resets in %lldh %lldm",
                  (long long)(left / 3600), (long long)((left % 3600) / 60));
}

void usage_app_widget_update(usage_app_widget_t *widget,
                             const provider_usage_t *usage, bool enabled,
                             bool clock_ready, int64_t now_epoch,
                             int64_t now_us)
{
    if (!widget || !widget->root || !usage) return;
    bool configured = enabled && usage->configured;
    if (configured) lv_obj_add_flag(widget->setup, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(widget->setup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *content[] = {widget->title, widget->state, widget->tokens,
                           widget->divider,
                           widget->window[0].name, widget->window[0].value,
                           widget->window[0].bar, widget->window[0].reset,
                           widget->window[1].name, widget->window[1].value,
                           widget->window[1].bar, widget->window[1].reset};
    for (size_t i = 0; i < sizeof(content) / sizeof(content[0]); ++i) {
        if (!content[i]) continue;
        if (configured) lv_obj_clear_flag(content[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(content[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!configured) return;
    int64_t age = now_us - usage->sampled_us;
    bool stale = !usage->has_sample || usage->sampled_us <= 0 ||
                 age < 0 || age > STALE_US;
    if (usage->status[0] && strcmp(usage->status, "Ready") != 0)
        lv_label_set_text(widget->state, usage->status);
    else if (usage->updating) lv_label_set_text(widget->state, "Updating...");
    else if (stale) lv_label_set_text(widget->state,
                                     usage->has_sample ? "Stale" : "Waiting for usage");
    else lv_label_set_text(widget->state, "");

    char text[48];
    if (usage->reset_credits_known) {
        snprintf(text, sizeof(text), "Reset tokens: %lu",
                 (unsigned long)usage->reset_credits);
        lv_label_set_text(widget->tokens, text);
        lv_obj_clear_flag(widget->tokens, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(widget->tokens, "");
        lv_obj_add_flag(widget->tokens, LV_OBJ_FLAG_HIDDEN);
    }
    int count = widget->weekly_only ? 1 : 2;
    for (int i = 0; i < count; ++i) {
        const quota_window_t *window = widget->weekly_only ?
            usage_provider_weekly_window(usage) : &usage->windows[i];
        if (widget->weekly_only)
            lv_label_set_text(widget->window[i].name,
                              window ? "Week left" : "");
        else if (usage->windows[i].window_seconds == 18000)
            lv_label_set_text(widget->window[i].name, "5h left");
        else if (usage->windows[i].window_seconds == 604800)
            lv_label_set_text(widget->window[i].name, "Week left");
        else lv_label_set_text(widget->window[i].name, "Window unavailable");
        if (window && window->valid && usage->has_sample) {
            lv_obj_clear_flag(widget->window[i].value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(widget->window[i].reset, LV_OBJ_FLAG_HIDDEN);
            float remaining = 100.0f - window->used_percent;
            snprintf(text, sizeof(text), "%.0f%%", remaining);
            lv_label_set_text(widget->window[i].value, text);
            lv_obj_set_width(widget->window[i].bar,
                (lv_coord_t)lroundf(remaining * widget->bar_width[i] / 100.0f));
            lv_obj_set_style_bg_color(widget->window[i].bar,
                lv_color_hex(display_quota_color(remaining)), 0);
            lv_obj_set_style_bg_opa(widget->window[i].bar,
                                     stale ? LV_OPA_50 : LV_OPA_COVER, 0);
            reset_text(text, sizeof(text), window, clock_ready, now_epoch);
            lv_label_set_text(widget->window[i].reset, text);
        } else {
            if (widget->weekly_only) {
                lv_label_set_text(widget->window[i].name, "Weekly usage unavailable");
                lv_obj_set_y(widget->window[i].name, 95);
                lv_obj_add_flag(widget->window[i].value, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(widget->window[i].reset, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_label_set_text(widget->window[i].value, "--%");
                lv_label_set_text(widget->window[i].reset, "Reset unknown");
            }
            lv_obj_set_width(widget->window[i].bar, 0);
        }
        if (widget->weekly_only && window)
            lv_obj_set_y(widget->window[i].name, 52);
    }
}
