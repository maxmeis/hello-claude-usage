#ifndef NATIVE_ESP_ERR_H
#define NATIVE_ESP_ERR_H
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERROR_CHECK(expr) do { if ((expr) != ESP_OK) abort(); } while (0)
#endif
