#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usage_provider.h"

#define CODEX_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_VERIFY_URL "https://auth.openai.com/codex/device"
#define CODEX_CALLBACK_URL "https://auth.openai.com/deviceauth/callback"
#define CODEX_ACCESS_SIZE 4096
#define CODEX_REFRESH_SIZE 2048
#define CODEX_ID_SIZE 4096

typedef struct {
    char device_auth_id[160];
    char user_code[32];
    uint32_t interval_sec;
} codex_device_code_t;

typedef struct {
    char authorization_code[2048];
    char code_verifier[256];
} codex_authorization_t;

typedef struct {
    char access[CODEX_ACCESS_SIZE];
    char refresh[CODEX_REFRESH_SIZE];
    char id[CODEX_ID_SIZE];
    char account_id[128];
    int64_t expires_at;
} codex_tokens_t;

bool codex_parse_device_code(const char *json, codex_device_code_t *out);
bool codex_parse_authorization(const char *json, codex_authorization_t *out);
/* Merge optional rotated fields into a copy of current. Initial exchange
 * passes an empty current. Account ID is always extracted from the ID token. */
bool codex_parse_tokens(const char *json, const codex_tokens_t *current,
                        int64_t now, codex_tokens_t *out);
bool codex_parse_usage(const char *json, int64_t sampled_at,
                       provider_usage_t *out);
bool codex_parse_retry_after(const char *value, uint32_t *seconds);
