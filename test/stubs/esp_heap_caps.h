#pragma once
#include <stdlib.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL 0
static inline size_t heap_caps_get_free_size(uint32_t c) { return 100000; }
static inline void *heap_caps_malloc(size_t s, uint32_t c) { return malloc(s); }
