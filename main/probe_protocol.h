#pragma once
#include <stdbool.h>
#include <stdint.h>

int probe_retry_after_seconds(const char *value, int64_t now);
bool probe_authorization_code(char *input, const char *expected_state, char **code);
