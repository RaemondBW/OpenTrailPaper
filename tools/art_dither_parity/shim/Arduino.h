#pragma once
// Host shim: just enough Arduino for src/media.cpp.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
inline uint32_t millis() { return 0; }
