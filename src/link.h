#pragma once

// The phone link: every app <-> device feature over one GATT service of two
// characteristics, carrying the protobuf messages of proto/link.proto.
// Design, framing, session and flows: docs/ble-link.md.
//
//   service b1c51000-9e0f-4b7a-9c6d-1f2e3a4b5c6d
//   RX      b1c51001-...  phone -> device, write / write without response
//   TX      b1c51002-...  device -> phone, notify
//
// The NimBLE host task only reassembles RX fragments and queues whole
// messages; the "link" task decodes, does the work and is the only sender on
// TX. Everything is per connection, and nothing is sent until the admitted
// phone said Hello. From then on (phone_link::active()) every device push
// goes on the link and the legacy characteristics stop notifying.

#include <stdint.h>

class NimBLEServer;

namespace phone_link {

// Creates the service (before the server starts). rxProperties: the
// legacy kW | kWNR set (encrypted + authenticated writes).
void begin(NimBLEServer* server, uint32_t rxProperties);

// FreeRTOS task body ("link", see main.cpp).
void task(void* arg);
constexpr uint32_t TASK_STACK = 8192;

// From ble_server's connection callbacks (NimBLE host task).
void onAdmitted();
void onDisconnected();

// This connection's app said Hello on the link: pushes go here.
bool active();

// Device-side changes for the link task to push (any task).
constexpr uint32_t PUSH_SETTINGS = 1u << 0;
constexpr uint32_t PUSH_MAP_LAYERS = 1u << 1;
constexpr uint32_t PUSH_MESH_CHANGED = 1u << 2;
void push(uint32_t what);
// A device media button with no AMS to take it: the app should act
// (sent only to an app that lists FEATURE_MEDIA_COMMAND_HANDLER).
void pushMediaCommand(uint8_t cmd);

}  // namespace phone_link
