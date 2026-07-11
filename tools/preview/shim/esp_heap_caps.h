#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_free(p) free(p)
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
