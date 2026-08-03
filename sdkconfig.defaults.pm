# sdkconfig.defaults for the PM-enabled framework rebuild.
#
# Use with the `framework = arduino, espidf` component build (see
# investigations/archive/cpu-sleep-spike.md). Rename to `sdkconfig.defaults` in
# the project root; the
# ESP-IDF builder picks it up automatically and rebuilds the affected components
# from source. These options do NOT take effect on the stock precompiled Arduino
# framework — that ships with PM compiled out.

# ---- Power management: automatic light sleep (no DFS) ----
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
CONFIG_PM_SLP_IRAM_OPT=y
CONFIG_PM_RTOS_IDLE_OPT=y
# We drive esp_pm_configure() with min==max==240 at runtime, so DFS is inert and
# the 80 MHz octal PSRAM timing (which assumes a 240 MHz-derived clock) stays
# valid. Do NOT lower the min freq unless PSRAM is first dropped to 40 MHz.
CONFIG_PM_DFS_INIT_AUTO=n

# ---- Keep the CPU at 240 MHz ----
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

# ---- Restore arduino-esp32's FreeRTOS timer task stack ----
# The stock Arduino framework ships 3120; lib-builder's defaults drop it to
# 2048, and that was the ONLY non-PM difference between the two sdkconfigs
# (verified by diff, 2026-08-01 — the rest is PM/tickless/BT-modem-sleep). A
# 2048-byte timer service task is a plausible source of the interrupt-watchdog
# reset seen on the first PM build, so keep it matched to stock.
CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=3120
CONFIG_TIMER_TASK_STACK_DEPTH=3120

# ---- Octal (OPI) PSRAM at 80 MHz — unchanged from the stock qio_opi variant ----
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_LEAKAGE_WORKAROUND=y

# ---- Flash: 16 MB, QIO, 80 MHz (matches the board JSON) ----
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y

# ---- Sleep leakage / correctness workarounds (already on in the stock cfg) ----
CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y
CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y

# ---- BLE controller modem sleep ----
# Modem sleep lets the controller power down its radio between connection
# events. For it to coexist with light sleep, the controller's low-power clock
# must keep running while the SoC sleeps: use the RTC slow clock, NOT the main
# XTAL (which is gated in light sleep and would force the controller to hold a
# no-light-sleep PM lock).
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y

# RTC slow-clock source. Internal 150 kHz RC needs no external part but drifts
# (~5%), which slightly widens BLE wake windows. If the board wires a 32.768 kHz
# crystal to the ESP32 XTAL_32K pins (VERIFY against the LilyGO schematic — the
# PCF8563 is a *separate I2C* RTC and does not count), prefer the two lines below
# instead for tighter, lower-power BLE timing:
#   CONFIG_RTC_CLK_SRC_EXT_CRYS=y
#   CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL=y
CONFIG_RTC_CLK_SRC_INT_RC=y

# ---- Partition table (matches vendor board JSON) ----
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="default_16MB.csv"

# ---- Core dump to flash partition (firmware already summarizes it on boot) ----
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
