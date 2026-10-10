#pragma once
#include <stddef.h>
namespace crash_report {
// Boot/task context only. Persist a report even if the platform saved no dump.
void begin(int reason, const char* reasonName, const char* firmware);
void tick();
// Main task, right after the SD card mounts (boot or late) and before it is
// exposed to a USB host: deliver queued reports and export the Memfault dump.
void onSdMounted(const char* why); // drain durable reports to SD after mount; retry without a card
void recordLine(const char* line, size_t length); // bounded RTC breadcrumbs
void requestTestPanic(); // deliberate diagnostic reset; refused while recording
void requestStatus(); // serial request, serviced on the main task
}
