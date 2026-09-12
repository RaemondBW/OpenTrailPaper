#pragma once
#include <stddef.h>
#include <stdint.h>

namespace memfault_service {
#if OT_MEMFAULT
void begin(int resetReason);
void tick();
void recordLine(const char* line, size_t length);
void requestStatus(bool exportSerial = false);
bool pending(size_t* size = nullptr);
const char* pendingId();
#else
inline void begin(int) {}
inline void tick() {}
inline void recordLine(const char*, size_t) {}
inline void requestStatus(bool = false) {}
inline bool pending(size_t* = nullptr) { return false; }
inline const char* pendingId() { return "disabled"; }
#endif
}
