#include "screensaver_registry.h"

#include <string.h>

void screensaver_registry_reset(screensaver_registry_t *registry)
{
    if (registry) memset(registry, 0, sizeof(*registry));
}

bool screensaver_registry_register(screensaver_registry_t *registry,
                                   const screensaver_descriptor_t *descriptor)
{
    if (!registry || !descriptor || descriptor->id == SAVER_ID_NONE || !descriptor->name ||
        registry->count >= SCREENSAVER_REGISTRY_CAPACITY ||
        screensaver_registry_find(registry, descriptor->id)) return false;
    registry->items[registry->count++] = *descriptor;
    return true;
}

const screensaver_descriptor_t *screensaver_registry_find(const screensaver_registry_t *registry,
                                                         saver_id_t id)
{
    if (!registry || id == SAVER_ID_NONE) return NULL;
    for (size_t i = 0; i < registry->count; ++i) {
        if (registry->items[i].id == id) return &registry->items[i];
    }
    return NULL;
}

const screensaver_descriptor_t *screensaver_registry_at(const screensaver_registry_t *registry,
                                                       size_t index)
{
    return registry && index < registry->count ? &registry->items[index] : NULL;
}

size_t screensaver_registry_count(const screensaver_registry_t *registry)
{
    return registry ? registry->count : 0;
}
