#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Starts a UART0 line reader. The caller must initialize Wi-Fi and OAuth first. */
bool usb_management_start(void);

/* Bounded request processing, exposed for the SDK-free protocol harness. */
bool usb_management_process_line(const char *line, size_t length,
                                 char *response, size_t response_size);
