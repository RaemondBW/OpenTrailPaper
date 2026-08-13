#pragma once

// Pin map for LilyGO T5S3 4.7" e-paper PRO (from LilyGO's factory firmware
// utilities.h). The e-paper data bus is handled internally by epdiy's
// epd_board_v7 definition and does not appear here.

// GPS (u-blox MIA-M10Q or L76K, autodetected at runtime)
#define BOARD_GPS_RXD       44
#define BOARD_GPS_TXD       43

// GPS acquisition telemetry over USB serial, for iterating on first-fix time.
// Prints a 1 Hz status line while searching (sats, SNR, DOP, fix type, aiding,
// system-vs-GPS time). Serial-only, so it doesn't spam the SD log. Cheap enough
// to leave on; set to 0 to silence.
#define GPS_DEBUG_SERIAL    1
// Echo every raw NMEA/UBX byte from the receiver to serial. Very noisy — turn
// on only for deep protocol debugging.
#define GPS_ECHO_NMEA       0

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
// SX1262 control lines (the data path is the shared SPI bus above). Every radio
// SPI transaction must be wrapped in sdLock()/sdUnlock() — see sd_bus.h; a LoRa
// transfer landing in the middle of an SD command corrupts the card.
#define BOARD_LORA_IRQ      10
#define BOARD_LORA_RST      1
#define BOARD_LORA_BUSY     47
// The module carries a TCXO on DIO3 and uses DIO2 to drive the RF switch
// (LilyGO's own examples: examples/lora_send, examples/sx1262_recv_mic_fcc).
#define BOARD_LORA_TCXO_V   2.4f

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

// Matched to the current release tag on purpose, even though this branch is NOT
// that release. The phone's updater compares this string to the latest GitHub
// release and offers an update on any difference, so a dev build reporting an
// older version invites a tap that silently replaces it with mainline firmware —
// which is exactly what happened once. Keep this in step with main's version
// while the branch is unmerged; on merge it goes back to being the real version.
#define FIRMWARE_VERSION    "v1.13"

// Rider settings
#define FTP_WATTS           250     // for the power zone bar (Coggan zones)
#define TIMEZONE_OFFSET_MINUTES (-420)  // clock display; PDT = UTC-7

// Map view: fallback center when there is no GPS fix (indoor testing).
// Alamo Square, San Francisco.
#define DEFAULT_MAP_LAT     37.7764
#define DEFAULT_MAP_LON     (-122.4346)

// Meshtastic mesh messaging (see docs/meshtastic.md).
//
// The region is a COMPILE-TIME choice because it is a legal one, not a
// preference: US_915 is 902-928 MHz and shipping a device that can be switched
// onto EU_868 from the phone would put it outside its certification. Change it
// here (and rebuild) for another region.
#define MESH_REGION_NAME    "US"
#define MESH_FREQ_START_MHZ 902.0f
#define MESH_FREQ_END_MHZ   928.0f
#define MESH_SPACING_MHZ    0.0f
// Region power limit is 30 dBm; the SX1262 tops out at 22, which is the cap that
// actually binds. Meshtastic's own US default is the same.
#define MESH_TX_DBM         22
// --- Channel name, and the modem it is spoken with -------------------------
//
// These are TWO INDEPENDENT settings and conflating them is a trap worth
// spelling out, because it costs a completely silent radio.
//
// The channel NAME decides where to listen. It feeds two hashes: djb2(name) picks
// the frequency slot, and xor(name) ^ xor(key) is the channel byte stamped on
// every packet. Nothing about the name says how fast to talk.
//
// The MODEM (spreading factor / bandwidth / coding rate) decides how. In the
// Meshtastic app it is chosen as a named "preset", and on a node whose channel
// name is EMPTY that preset name is also used as the channel name — which is why
// the two look like one setting until you meet a node where they differ. A
// channel called "MediumFast" running the LongFast modem is perfectly normal, and
// matching only one of the two leaves you deaf: same frequency but the wrong
// spreading factor demodulates nothing at all.
//
// So set the name to match the other node's CHANNEL, and the modem to match its
// PRESET. Ask the device what it ended up with over the serial console: `mesh`.
//
// Both are DEFAULTS ONLY — the phone can change either at runtime (Mesh tab ->
// antenna button), and once it has, the stored choice wins. These are what a
// device that has never been configured comes up on, so they are Meshtastic's
// own defaults: a node fresh out of a box is on exactly this, which is what makes
// two unconfigured devices able to find each other.
//
//   name          slot  frequency (US)   channel hash (default key)
//   LongFast        19    906.875 MHz        0x08
//   MediumFast      44    913.125 MHz        0x1f
//
// There is no default channel NAME: with none set, the name comes from the modem
// preset below, which is what a stock Meshtastic node does (it leaves its primary
// channel unnamed). That is what keeps preset and channel in step — a node on the
// MediumFast preset is on MediumFast's slot, not LongFast's. A name is only stored
// when the rider sets one, for a private channel.
//
// Index into mesh::kPresets (mesh_proto.cpp). 0 = LongFast, SF11 / 250 kHz —
// Meshtastic's default, so an unconfigured device lands where stock nodes are.
#define MESH_PRESET_DEFAULT 0
// Fixed by the protocol, not tunable: every Meshtastic node uses this sync word
// and preamble, so a mismatch here means silence in both directions.
#define MESH_SYNC_WORD      0x2B
#define MESH_PREAMBLE_LEN   16
// Hops a message we originate may take across the mesh. Meshtastic's own default
// is 3; the Bay Area Mesh asks personal/chat nodes for 6 ("up to 7 is ok if
// desired"), which is what this is set to. It costs other people's airtime rather
// than ours — we do not relay — so it is a community norm, not a preference, and
// worth checking against the mesh you are actually on. The field is 3 bits, so 7
// is the ceiling.
//   https://bayme.sh/docs/getting-started/recommended-settings/
#define MESH_HOP_LIMIT      6
// Sharing our own position on a channel (off unless the rider turns it on, per
// channel). The thresholds follow the Bay Area Mesh's smart-position guidance —
// "minimum 10 minutes" with a "100 to 130" metre trigger — because a position
// broadcast is spent on everyone else's airtime: it goes out on the shared
// frequency and other nodes relay it, whether or not they can decrypt it.
//   https://bayme.sh/docs/getting-started/recommended-settings/
// A rider moving at 20 km/h covers ~3 km between updates, which is the price of
// being a good neighbour on a mesh you share.
#define MESH_POS_MIN_INTERVAL_MS (10UL * 60UL * 1000UL)
#define MESH_POS_MIN_MOVE_M      100.0f
// Re-send even when parked, so somebody who has just joined the channel can see
// where you are without waiting for you to move.
#define MESH_POS_HEARTBEAT_MS    (60UL * 60UL * 1000UL)

// How many received/sent messages the device keeps for the phone to pull.
#define MESH_MSG_HISTORY    48
// How many mesh neighbours we remember names for.
#define MESH_NODE_MAX       32

// Ride recording
#define RIDE_DIR            "/rides"
#define RECORD_INTERVAL_MS  1000
#define FIT_FLUSH_EVERY_S   15
// Rides smaller than this are stubs (an accidental start/stop with under a
// minute of data) and are hidden from the history list — a FIT file is ~284
// fixed bytes plus ~25 per 1 Hz record (see fit_writer.cpp).
#define RIDE_MIN_USEFUL_BYTES 1500
