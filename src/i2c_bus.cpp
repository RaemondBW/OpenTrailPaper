// Hold light sleep off across EVERY I2C transaction, whoever issues it.
//
// i2cLock()/i2cUnlock() (i2c_bus.h) already do this for our own callers, but
// the panel driver (EPD_Painter: TPS65185 rails, PCA9535 expander, its power
// guard and idle-off) talks on the same Wire behind its own mutex and never
// takes i2cLock(). PR #89 covered its paints with a timed hold; its
// interrupt-driven expander polls and rail checks stayed uncovered, and on
// 2026-10-02 two more rides reset with core 0 in i2c_isr_handler_default
// while core 1 slept (memfault db1e825a, f389cddb; both current task "gps").
//
// The IDF driver takes only an APB-frequency PM lock for a transaction
// (i2c.c), which stops DFS but not light sleep, so the other core can gate the
// peripheral mid-byte. A peripheral that resumes with a status the ISR cannot
// clear re-asserts its level-1 interrupt forever and starves the tick that
// feeds the interrupt watchdog. Wrapping the Arduino HAL entry points — the
// one path every TwoWire user shares — closes that for all of them at once.
// Linker: -Wl,--wrap=i2cWrite/i2cRead/i2cWriteReadNonStop (platformio.ini).

#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include "power_mgmt.h"

extern "C" {
esp_err_t __real_i2cWrite(uint8_t i2c_num, uint16_t address, const uint8_t* buff, size_t size, uint32_t timeOutMillis);
esp_err_t __real_i2cRead(uint8_t i2c_num, uint16_t address, uint8_t* buff, size_t size, uint32_t timeOutMillis, size_t* readCount);
esp_err_t __real_i2cWriteReadNonStop(uint8_t i2c_num, uint16_t address, const uint8_t* wbuff, size_t wsize, uint8_t* rbuff, size_t rsize, uint32_t timeOutMillis, size_t* readCount);

esp_err_t __wrap_i2cWrite(uint8_t i2c_num, uint16_t address, const uint8_t* buff, size_t size, uint32_t timeOutMillis) {
    power_mgmt::busyAcquire();
    const esp_err_t r = __real_i2cWrite(i2c_num, address, buff, size, timeOutMillis);
    power_mgmt::busyRelease();
    return r;
}

esp_err_t __wrap_i2cRead(uint8_t i2c_num, uint16_t address, uint8_t* buff, size_t size, uint32_t timeOutMillis, size_t* readCount) {
    power_mgmt::busyAcquire();
    const esp_err_t r = __real_i2cRead(i2c_num, address, buff, size, timeOutMillis, readCount);
    power_mgmt::busyRelease();
    return r;
}

esp_err_t __wrap_i2cWriteReadNonStop(uint8_t i2c_num, uint16_t address, const uint8_t* wbuff, size_t wsize, uint8_t* rbuff, size_t rsize, uint32_t timeOutMillis, size_t* readCount) {
    power_mgmt::busyAcquire();
    const esp_err_t r = __real_i2cWriteReadNonStop(i2c_num, address, wbuff, wsize, rbuff, rsize, timeOutMillis, readCount);
    power_mgmt::busyRelease();
    return r;
}
}
