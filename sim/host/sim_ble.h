// Internals of the sim's BLE stack shared by nimble_sim.cpp (the stack and
// the simble server) and sensors_sim.cpp (virtual sensors).
#pragma once
#include <NimBLEDevice.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace simble {

// The NimBLE host task: every stack callback into the firmware runs here, one
// at a time, as on the device.
void post(std::function<void()> fn);
void postAfter(uint32_t simMs, std::function<void()> fn);

std::string uuidBytes(const NimBLEUUID& u);      // 16 bytes, string order
NimBLEUUID uuidFrom(const uint8_t* b16);

struct SimRemoteChar {
    NimBLEUUID svc, uuid;
    uint32_t props = 0;
    std::vector<uint8_t> value;
    NimBLERemoteCharacteristic* subscriber = nullptr;
};

// Something the device connects to as a central: a virtual sensor, or the
// connected phone's own GATT server (AMS, the GAP name).
struct SimPeripheral {
    NimBLEAddress addr;
    std::string name, mfg;
    std::vector<NimBLEUUID> advUuids;
    int rssi = -55;
    bool advertising = true;
    std::vector<std::shared_ptr<SimRemoteChar>> chars;
    NimBLEClient* client = nullptr;   // the device's client connected to it
    uint16_t phoneHandle = BLE_HS_CONN_HANDLE_NONE;   // set for the phone
    std::shared_ptr<SimRemoteChar> find(const NimBLEUUID& svc, const NimBLEUUID& chr);
};

std::mutex& peripheralsMutex();
std::vector<std::shared_ptr<SimPeripheral>>& peripherals();
std::shared_ptr<SimPeripheral> findPeripheral(const NimBLEAddress& a);

// A peripheral's characteristic changed: notify a subscribed client.
void peripheralNotify(const std::shared_ptr<SimPeripheral>& p, const NimBLEUUID& svc,
                      const NimBLEUUID& chr, const uint8_t* data, size_t len);
// It went out of range: drop a connected client.
void peripheralGone(const std::shared_ptr<SimPeripheral>& p, int reason);

// Phone GATT reads for AMS go over the phone's simble socket.
bool phoneRead(uint16_t handle, const NimBLEUUID& svc, const NimBLEUUID& chr,
               std::vector<uint8_t>& out);
void phoneSubscribe(uint16_t handle, const NimBLEUUID& svc, const NimBLEUUID& chr, uint16_t cccd);
void phoneWrite(uint16_t handle, const NimBLEUUID& svc, const NimBLEUUID& chr,
                const uint8_t* data, size_t len);

// Control channel hooks.
void dropPhone(int hciReason);   // the radio link to the phone is lost
std::string statusJson();

}  // namespace simble
