#include "codex_protocol.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static bool copy_field(const cJSON *object, const char *key, char *dest,
                       size_t capacity, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return !required;
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t length = strlen(item->valuestring);
    if (!length || length >= capacity) return false;
    memcpy(dest, item->valuestring, length + 1);
    return true;
}

static bool integer_field(const cJSON *object, const char *key, int64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0 || item->valuedouble > 9007199254740991.0 ||
        floor(item->valuedouble) != item->valuedouble) return false;
    *out = (int64_t)item->valuedouble;
    return true;
}

bool codex_parse_device_code(const char *json, codex_device_code_t *out)
{
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    int64_t interval = 5; /* The official response makes interval optional. */
    const cJSON *snake = cJSON_GetObjectItemCaseSensitive(root, "user_code");
    const cJSON *legacy = cJSON_GetObjectItemCaseSensitive(root, "usercode");
    bool aliases_match = !snake || !legacy ||
        (cJSON_IsString(snake) && cJSON_IsString(legacy) &&
         snake->valuestring && legacy->valuestring &&
         strcmp(snake->valuestring, legacy->valuestring) == 0);
    bool ok = cJSON_IsObject(root) && aliases_match && (snake || legacy) &&
        copy_field(root, "device_auth_id", out->device_auth_id,
                   sizeof(out->device_auth_id), true) &&
        copy_field(root, snake ? "user_code" : "usercode", out->user_code,
                   sizeof(out->user_code), true);
    const cJSON *raw = cJSON_GetObjectItemCaseSensitive(root, "interval");
    if (ok && !raw) {
        /* Avoid a busy poll if an older issuer omits the optional hint. */
    } else if (ok && cJSON_IsString(raw) && raw->valuestring) {
        char *end = NULL;
        unsigned long n = strtoul(raw->valuestring, &end, 10);
        while (end && *end == ' ') ++end;
        ok = end && end != raw->valuestring && !*end && n >= 1 && n <= 300;
        interval = (int64_t)n;
    } else if (ok) {
        ok = integer_field(root, "interval", &interval) && interval >= 1 && interval <= 300;
    }
    if (ok) out->interval_sec = (uint32_t)interval;
    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

bool codex_parse_authorization(const char *json, codex_authorization_t *out)
{
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    bool ok = cJSON_IsObject(root) &&
        copy_field(root, "authorization_code", out->authorization_code,
                   sizeof(out->authorization_code), true) &&
        copy_field(root, "code_verifier", out->code_verifier,
                   sizeof(out->code_verifier), true);
    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

static int base64_digit(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

static cJSON *jwt_payload(const char *jwt)
{
    const char *dot = strchr(jwt, '.');
    if (!dot) return NULL;
    const char *end = strchr(dot + 1, '.');
    if (!end || end - dot < 2 || end - dot > 4096) return NULL;
    size_t length = (size_t)(end - dot - 1);
    char *decoded = calloc(length + 1, 1);
    if (!decoded) return NULL;
    unsigned accumulator = 0, bits = 0;
    size_t written = 0;
    bool ok = true;
    for (const char *p = dot + 1; p < end; ++p) {
        int value = base64_digit(*p);
        if (value < 0) { ok = false; break; }
        accumulator = (accumulator << 6) | (unsigned)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded[written++] = (char)((accumulator >> bits) & 255u);
        }
    }
    decoded[written] = 0;
    cJSON *root = ok ? cJSON_Parse(decoded) : NULL;
    memset(decoded, 0, length + 1);
    free(decoded);
    return root;
}

static bool account_id_from_jwt(const char *jwt, char *out, size_t capacity)
{
    cJSON *root = jwt_payload(jwt);
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(root, "https://api.openai.com/auth");
    bool ok = cJSON_IsObject(auth) &&
        copy_field(auth, "chatgpt_account_id", out, capacity, true);
    cJSON_Delete(root);
    return ok;
}

static bool expiration_from_jwt(const char *jwt, int64_t now, int64_t *expires_at)
{
    cJSON *root = jwt_payload(jwt);
    int64_t exp;
    bool ok = cJSON_IsObject(root) && integer_field(root, "exp", &exp) &&
        exp > now && exp <= now + 31536000;
    if (ok) *expires_at = exp;
    cJSON_Delete(root);
    return ok;
}

bool codex_parse_tokens(const char *json, const codex_tokens_t *current,
                        int64_t now, codex_tokens_t *out)
{
    if (!json || !out || now < 1735689600 || current == out) return false;
    memset(out, 0, sizeof(*out));
    if (current) memcpy(out, current, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    int64_t lifetime = 0;
    bool ok = cJSON_IsObject(root) &&
        copy_field(root, "access_token", out->access, sizeof(out->access), !current) &&
        copy_field(root, "refresh_token", out->refresh, sizeof(out->refresh), !current) &&
        copy_field(root, "id_token", out->id, sizeof(out->id), !current) &&
        out->access[0] && out->refresh[0] && out->id[0] &&
        account_id_from_jwt(out->id, out->account_id, sizeof(out->account_id));
    if (ok && integer_field(root, "expires_in", &lifetime) && lifetime > 0 && lifetime <= 31536000)
        out->expires_at = now + lifetime;
    else if (ok && !expiration_from_jwt(out->access, now, &out->expires_at))
        ok = false;
    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

static void parse_window(const cJSON *object, quota_window_t *window)
{
    if (!cJSON_IsObject(object)) return;
    const cJSON *used = cJSON_GetObjectItemCaseSensitive(object, "used_percent");
    int64_t duration, reset;
    if (!cJSON_IsNumber(used) || !isfinite(used->valuedouble) ||
        used->valuedouble < 0 || used->valuedouble > 100 ||
        !integer_field(object, "limit_window_seconds", &duration) ||
        duration < 1 || duration > 31536000 ||
        !integer_field(object, "reset_at", &reset) || reset < 1735689600) return;
    window->valid = true;
    window->used_percent = (float)used->valuedouble;
    window->window_seconds = (uint32_t)duration;
    window->resets_at = reset;
}

bool codex_parse_usage(const char *json, int64_t sampled_at, provider_usage_t *out)
{
    if (!json || !out || sampled_at < 1735689600) return false;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    const cJSON *limit = cJSON_GetObjectItemCaseSensitive(root, "rate_limit");
    if (!cJSON_IsObject(root) || (!cJSON_IsObject(limit) && !cJSON_IsNull(limit))) {
        cJSON_Delete(root); return false;
    }
    parse_window(cJSON_GetObjectItemCaseSensitive(limit, "primary_window"), &out->windows[0]);
    parse_window(cJSON_GetObjectItemCaseSensitive(limit, "secondary_window"), &out->windows[1]);
    const cJSON *credits = cJSON_GetObjectItemCaseSensitive(root, "rate_limit_reset_credits");
    int64_t count;
    if (cJSON_IsObject(credits) && integer_field(credits, "available_count", &count) &&
        count <= UINT32_MAX) {
        out->reset_credits_known = true;
        out->reset_credits = (uint32_t)count;
    }
    out->configured = true;
    out->has_sample = true;
    out->sampled_at = sampled_at;
    cJSON_Delete(root);
    return true;
}

bool codex_parse_retry_after(const char *value, uint32_t *seconds)
{
    if (!value || !seconds || !isdigit((unsigned char)value[0])) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end || parsed > 86400) return false;
    *seconds = (uint32_t)parsed;
    return true;
}
