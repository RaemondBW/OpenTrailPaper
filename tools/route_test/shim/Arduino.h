// Host shim: the sliver of Arduino.h that routes.cpp needs.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

inline double radians(double deg) { return deg * M_PI / 180.0; }

class SerialStub {
  public:
    template <typename... A> void printf(const char*, A...) {}
    void println(const char* = "") {}
};
static SerialStub Serial;
