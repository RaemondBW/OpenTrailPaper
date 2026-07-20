#pragma once

// Pin map for LilyGO T5S3 4.7" e-paper PRO (from LilyGO's factory firmware
// utilities.h). The e-paper data bus is handled internally by epdiy's
// epd_board_v7 definition and does not appear here.

// GPS (u-blox MIA-M10Q or L76K, autodetected at runtime)
#define BOARD_GPS_RXD       44
#define BOARD_GPS_TXD       43

// Shared I2C bus: touch (GT911), RTC (PCF8563), fuel gauge (BQ27220),
// charger (BQ25896), IO expander (XL9555/PCA9535)
#define BOARD_SDA           39
#define BOARD_SCL           40

#define BOARD_TOUCH_INT     3
#define BOARD_TOUCH_RST     9

// SD card (shared SPI bus with LoRa)
#define BOARD_SPI_MISO      21
#define BOARD_SPI_MOSI      13
#define BOARD_SPI_SCLK      14
#define BOARD_SD_CS         12
#define BOARD_LORA_CS       46

#define BOARD_PCA9535_INT   38
#define BOARD_BOOT_BTN      0

// Backlight: PT4103 driver enabled by GPIO11 (PWM brightness).
#define BOARD_BL_EN         11
// Front button on GPIO48 (free — epdiy drives the panel CKV via the LCD
// peripheral, not this pin). Cycles the backlight brightness.
#define BOARD_BL_BTN        48

// GPS + LoRa 3V3 rail is gated by IO0 on the XL9555 expander.
#define IOEXP_PIN_RADIO_POWER 0
// Side button: expander PC12 (pin 10), pressed = LOW
#define IOEXP_PIN_SIDE_BUTTON 10

// Display orientation: 540x960 portrait, matching the factory firmware
// (touch coords map 1:1 in this rotation). Use EPD_ROT_PORTRAIT to flip 180°.
#define DISPLAY_ROTATION    EPD_ROT_INVERTED_PORTRAIT

// Full (ghost-clearing) refresh every N fast refreshes.
#define FULL_REFRESH_EVERY  60

#define FIRMWARE_VERSION    "v0.84"

// Rider settings
#define FTP_WATTS           250     // for the power zone bar (Coggan zones)
#define TIMEZONE_OFFSET_MINUTES (-420)  // clock display; PDT = UTC-7

// Map view: fallback center when there is no GPS fix (indoor testing).
// Alamo Square, San Francisco.
#define DEFAULT_MAP_LAT     37.7764
#define DEFAULT_MAP_LON     (-122.4346)

// Ride recording
#define RIDE_DIR            "/rides"
#define RECORD_INTERVAL_MS  1000
#define FIT_FLUSH_EVERY_S   15
// Rides smaller than this are stubs (an accidental start/stop with under a
// minute of data) and are hidden from the history list — a FIT file is ~284
// fixed bytes plus ~25 per 1 Hz record (see fit_writer.cpp).
#define RIDE_MIN_USEFUL_BYTES 1500
