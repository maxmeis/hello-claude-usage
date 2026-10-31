#ifndef HELLO_BOX_SCREENSAVER_REGISTRY_H
#define HELLO_BOX_SCREENSAVER_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint16_t saver_id_t;
enum { SAVER_ID_NONE = 0, SAVER_ID_WORD_CLOCK = 1 };
#define SCREENSAVER_REGISTRY_CAPACITY 8u

typedef struct {
    saver_id_t id;
    const char *name;
    void *context;
    void (*mount)(void *context, void *parent);
    void (*update)(void *context);
    void (*set_visible)(void *context, bool visible);
} screensaver_descriptor_t;

/* Descriptors are copied; their strings and callback contexts remain caller-owned. */
typedef struct {
    screensaver_descriptor_t items[SCREENSAVER_REGISTRY_CAPACITY];
    size_t count;
} screensaver_registry_t;

void screensaver_registry_reset(screensaver_registry_t *registry);
bool screensaver_registry_register(screensaver_registry_t *registry,
                                   const screensaver_descriptor_t *descriptor);
const screensaver_descriptor_t *screensaver_registry_find(const screensaver_registry_t *registry,
                                                         saver_id_t id);
const screensaver_descriptor_t *screensaver_registry_at(const screensaver_registry_t *registry,
                                                       size_t index);
size_t screensaver_registry_count(const screensaver_registry_t *registry);

#endif
