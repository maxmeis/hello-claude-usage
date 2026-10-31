#include "provider_http.h"

#include "freertos/semphr.h"

static SemaphoreHandle_t s_http_mutex;

bool provider_http_init(void)
{
    if (!s_http_mutex) s_http_mutex = xSemaphoreCreateMutex();
    return s_http_mutex != NULL;
}

bool provider_http_take(TickType_t timeout)
{
    return s_http_mutex && xSemaphoreTake(s_http_mutex, timeout) == pdTRUE;
}

void provider_http_give(void)
{
    if (s_http_mutex) xSemaphoreGive(s_http_mutex);
}
