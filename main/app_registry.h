#ifndef HELLO_BOX_APP_REGISTRY_H
#define HELLO_BOX_APP_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint16_t app_id_t;
enum { APP_ID_NONE = 0, APP_ID_ALL = 0, APP_ID_CLAUDE = 1, APP_ID_CODEX = 2 };
enum { APP_CAP_REFRESH = 1u << 0, APP_CAP_INTERRUPT_SAVER = 1u << 1 };
#define APP_REGISTRY_CAPACITY 8u

typedef struct {
    app_id_t id;
    const char *name;
    uint32_t capabilities;
    void *context;
    bool (*enabled)(void *context);
    bool (*configured)(void *context);
    bool (*request_refresh)(void *context);
    void (*mount_widget)(void *context, void *parent);
    void (*mount_app)(void *context, void *parent);
    void (*update_views)(void *context);
} app_descriptor_t;

/* Descriptors are copied; their strings and callback contexts remain caller-owned. */
typedef struct {
    app_descriptor_t items[APP_REGISTRY_CAPACITY];
    size_t count;
} app_registry_t;

void app_registry_reset(app_registry_t *registry);
bool app_registry_register(app_registry_t *registry, const app_descriptor_t *descriptor);
const app_descriptor_t *app_registry_find(const app_registry_t *registry, app_id_t id);
const app_descriptor_t *app_registry_at(const app_registry_t *registry, size_t index);
size_t app_registry_count(const app_registry_t *registry);
bool app_registry_is_enabled(const app_registry_t *registry, app_id_t id);
bool app_registry_is_configured(const app_registry_t *registry, app_id_t id);
bool app_registry_request_refresh(const app_registry_t *registry, app_id_t id);

#endif
