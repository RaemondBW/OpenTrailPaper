// The panel's touch controller in the sim: getPoint() reports what the
// control channel injected (sim_touch_*), and a home-key press fires the
// callback the firmware registered, as the GT911 does inside getPoint().
#pragma once
#include <Arduino.h>
#include <Wire.h>
#define GT911_SLAVE_ADDRESS_L 0x5D
#define GT911_SLAVE_ADDRESS_H 0x14
typedef void (*home_button_callback_t)(void* user);
class TouchDrvGT911 {
public:
    void setPins(int rst, int irq) { (void)rst; (void)irq; }
    bool begin(TwoWire& w, uint8_t addr, int sda, int scl) { (void)w; (void)addr; (void)sda; (void)scl; return true; }
    uint8_t getPoint(int16_t& x, int16_t& y, uint8_t max = 1);
    uint8_t getPoint(int16_t* x, int16_t* y, uint8_t max = 1) { return getPoint(*x, *y, max); }
    bool isPressed();
    void setHomeButtonCallback(home_button_callback_t cb, void* user) { homeCb_ = cb; homeUser_ = user; }
    void sleep() {}
    void wakeup() {}
    void setMaxCoordinates(uint16_t, uint16_t) {}
    void setSwapXY(bool) {}
    void setMirrorXY(bool, bool) {}
    void setInterruptMode(uint8_t) {}
    const char* getModelName() { return "GT911 (sim)"; }
    home_button_callback_t homeCb_ = nullptr;
    void* homeUser_ = nullptr;
};
