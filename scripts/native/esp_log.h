#ifndef NATIVE_ESP_LOG_H
#define NATIVE_ESP_LOG_H

/* Host renders have no ESP-IDF logger. */
#define ESP_LOGI(tag, format, ...) ((void)0)
#define ESP_LOGW(tag, format, ...) ((void)0)
#define ESP_LOGE(tag, format, ...) ((void)0)

#endif
