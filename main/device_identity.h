#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include "esp_err.h"

/* Call after nvs_flash_init(). A successful identity never changes in RAM. */
esp_err_t device_identity_init(void);
const char *device_identity_name(void);

#endif
