# ESP-IDF Bluetooth controller adapter

`bt.c` is unmodified Espressif source from ESP-IDF v4.4.6, commit
`3572900934`, matching this project's Arduino 2.0.14 SDK `versions.txt`.
ESP32-S3 shares the controller adapter under the `esp32c3` source directory.

Source: https://github.com/espressif/esp-idf/blob/3572900934/components/bt/controller/esp32c3/bt.c

SHA-256: `47625a3558e51afe0cf11588907228b0fe9bed9bdcdeca88e70e9ea4c01553ad`

License: Apache-2.0, reproduced in `LICENSE`.

Only `t5s3-painter-ble-xtal` compiles this source, through
`src/ble_clock_xtal.c`. It selects the main crystal and retains that crystal
during light sleep, using the upstream controller's supported configuration.
The application init entry point also changes the caller's sleep-clock field
to match. Other SDK components, private controller binaries and installed
framework files remain untouched. Application definitions satisfy all references
to this adapter so the old `libbt.a(bt.c.obj)` must not be pulled into the link.
The post-link verification enforces that and the source fingerprint.

The baseline environment continues to link its original controller adapter.
