#pragma once
#include <stddef.h>

#define MALLOC_CAP_SPIRAM 0x400
#define MALLOC_CAP_8BIT 0x04

void *heap_caps_malloc(size_t bytes, unsigned capabilities);
