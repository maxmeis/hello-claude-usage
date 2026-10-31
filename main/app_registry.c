#include "app_registry.h"

#include <string.h>

void app_registry_reset(app_registry_t *registry)
{
    if (registry) memset(registry, 0, sizeof(*registry));
}

bool app_registry_register(app_registry_t *registry, const app_descriptor_t *descriptor)
{
    if (!registry || !descriptor || descriptor->id == APP_ID_NONE || !descriptor->name ||
        registry->count >= APP_REGISTRY_CAPACITY ||
        app_registry_find(registry, descriptor->id)) return false;
    registry->items[registry->count++] = *descriptor;
    return true;
}

const app_descriptor_t *app_registry_find(const app_registry_t *registry, app_id_t id)
{
    if (!registry || id == APP_ID_NONE) return NULL;
    for (size_t i = 0; i < registry->count; ++i) {
        if (registry->items[i].id == id) return &registry->items[i];
    }
    return NULL;
}

const app_descriptor_t *app_registry_at(const app_registry_t *registry, size_t index)
{
    return registry && index < registry->count ? &registry->items[index] : NULL;
}

size_t app_registry_count(const app_registry_t *registry)
{
    return registry ? registry->count : 0;
}

bool app_registry_is_enabled(const app_registry_t *registry, app_id_t id)
{
    const app_descriptor_t *app = app_registry_find(registry, id);
    return app && app->enabled && app->enabled(app->context);
}

bool app_registry_is_configured(const app_registry_t *registry, app_id_t id)
{
    const app_descriptor_t *app = app_registry_find(registry, id);
    return app && app->configured && app->configured(app->context);
}

bool app_registry_request_refresh(const app_registry_t *registry, app_id_t id)
{
    const app_descriptor_t *app = app_registry_find(registry, id);
    return app && (app->capabilities & APP_CAP_REFRESH) && app->request_refresh &&
           app_registry_is_enabled(registry, id) &&
           app_registry_is_configured(registry, id) && app->request_refresh(app->context);
}
