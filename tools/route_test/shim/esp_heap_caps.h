#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 0
inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }
