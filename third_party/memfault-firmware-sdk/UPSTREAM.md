# Memfault SDK source subset

Unmodified files from https://github.com/memfault/memfault-firmware-sdk at
`db3472ce1f1e355cc540475456d01518940ab43b` (SDK 1.44.0).
See LICENSE for redistribution and service integration terms.

Includes core/panics/util, ESP-IDF 4.x panic and flash storage ports, headers,
and the official ELF build-ID tool. Network, heartbeat metrics, and automatic
upload ports are not built. `tools/memfault_build.py` selects source files.
Project-specific boot, regions, and offline transport live under src/.
