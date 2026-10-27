#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

#include <stdbool.h>

#include "esp_err.h"

/* Commit the durable reset journal. Does not erase settings or restart. */
esp_err_t factory_reset_request(void);

/* Resume app storage cleanup after NVS init, before device identity init. */
esp_err_t factory_reset_prepare_boot(void);

/* A missing journal means no reset; malformed or unreadable data is an error. */
esp_err_t factory_reset_is_pending(bool *out);

/* Clear the journal only after esp_wifi_restore() succeeds. */
esp_err_t factory_reset_complete(void);

#endif
