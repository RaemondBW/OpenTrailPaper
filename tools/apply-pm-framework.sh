#!/bin/sh
# Overlay the PM-enabled (CONFIG_PM_ENABLE + tickless idle) ESP32-S3 SDK onto
# the installed framework-arduinoespressif32 package, so builds get a real
# esp_pm_configure() instead of the stock 8-byte ESP_ERR_NOT_SUPPORTED stub.
#
# Why this exists: `framework = arduino` links PRECOMPILED archives; no build
# flag can enable PM (see investigations/pm-rebuild-baseline.md). The libs were
# rebuilt once by .github/workflows/build-pm-libs.yml and are pinned as a
# GitHub prerelease asset — prerelease, so /releases/latest (the OTA checks in
# both companion apps) and the Pages flasher (filters on firmware.bin) never
# see it.
#
# Idempotent: exits 0 immediately if the framework is already PM-enabled, so
# it is safe on a CI cache hit that restored an overlaid ~/.platformio.
#
# Usage:  tools/apply-pm-framework.sh [path-to-tarball]
#   With no argument, downloads the pinned release asset.
set -eu

TARBALL_URL="https://github.com/RaemondBW/OpenTrailPaper/releases/download/pm-libs-1/pm-framework-esp32s3.tar.gz"
TARBALL_SHA256="2a9e19e5ae168231f41b147a989da67dd0d533a1ad7de1313b27d123691bdfc0"
FRAMEWORK="${PM_FRAMEWORK_DIR:-$HOME/.platformio/packages/framework-arduinoespressif32}"

[ -d "$FRAMEWORK/tools/sdk/esp32s3" ] || {
  echo "apply-pm-framework: no framework at $FRAMEWORK — run 'pio pkg install' first" >&2
  exit 1
}

# The fastest "which framework am I on?" test there is: on stock,
# esp_pm_configure in libesp_pm.a is an 8-byte stub; PM-enabled it is ~220 B.
pm_size() {
  python3 - "$FRAMEWORK/tools/sdk/esp32s3/lib/libesp_pm.a" <<'PY'
import re, sys
data = open(sys.argv[1], "rb").read()
# ar member header for esp_pm.c.obj is enough context: find the ELF that
# defines esp_pm_configure and read its symbol size from the symtab. Cheaper
# and dependency-free vs. requiring the xtensa binutils on the runner.
import io, struct
def elf_symbols(blob):
    if blob[:4] != b"\x7fELF": return
    is64 = blob[4] == 2
    (e_shoff,) = struct.unpack_from("<Q" if is64 else "<I", blob, 0x28 if is64 else 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", blob, 0x3a if is64 else 0x2e)
    shs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if is64:
            name, typ, _, _, offset, size, link, _, _, entsize = struct.unpack_from("<IIQQQQIIQQ", blob, off)
        else:
            name, typ, _, _, offset, size, link, _, _, entsize = struct.unpack_from("<10I", blob, off)
        shs.append((typ, offset, size, link, entsize))
    for typ, offset, size, link, entsize in shs:
        if typ != 2 or not entsize:  # SHT_SYMTAB
            continue
        strtab_off, strtab_size = shs[link][1], shs[link][2]
        strtab = blob[strtab_off:strtab_off + strtab_size]
        for j in range(size // entsize):
            so = offset + j * entsize
            if is64:
                nameoff, _, _, _, _, ssize = struct.unpack_from("<IBBHQQ", blob, so)
            else:
                nameoff, _, ssize, _, _, _ = struct.unpack_from("<IIIBBH", blob, so)
            end = strtab.find(b"\0", nameoff)
            yield strtab[nameoff:end].decode("latin1"), ssize

# Walk ar members (header-aligned to 2), checking each ELF object.
pos = 8
while pos + 60 <= len(data):
    size = int(data[pos+48:pos+58].split()[0] or 0)
    body = data[pos+60:pos+60+size]
    for name, ssize in elf_symbols(body) or ():
        if name == "esp_pm_configure":
            print(ssize)
            sys.exit(0)
    pos += 60 + size + (size & 1)
print(0)
PY
}

size_before=$(pm_size)
if [ "$size_before" -gt 100 ]; then
  echo "apply-pm-framework: already PM-enabled (esp_pm_configure=${size_before}B) — nothing to do"
  exit 0
fi

TARBALL="${1:-}"
cleanup=""
if [ -z "$TARBALL" ]; then
  TARBALL=$(mktemp /tmp/pm-framework.XXXXXX.tar.gz)
  cleanup="$TARBALL"
  echo "apply-pm-framework: downloading $TARBALL_URL"
  curl -fsSL --retry 3 -o "$TARBALL" "$TARBALL_URL"
fi

echo "$TARBALL_SHA256  $TARBALL" | shasum -a 256 -c - >/dev/null || {
  echo "apply-pm-framework: tarball sha256 mismatch — refusing to install" >&2
  [ -n "$cleanup" ] && rm -f "$cleanup"
  exit 1
}

# Whole-tree replace, never a partial overlay: lib/ and the flash-variant dirs
# (qio_opi/ here) must come from the same lib-builder run or FreeRTOS and the
# drivers disagree about tickless idle (pm-rebuild-baseline.md §3).
rm -rf "$FRAMEWORK/tools/sdk/esp32s3"
tar -xzf "$TARBALL" -C "$FRAMEWORK"
[ -n "$cleanup" ] && rm -f "$cleanup"

grep -q "^CONFIG_PM_ENABLE=y" "$FRAMEWORK/tools/sdk/esp32s3/sdkconfig" || {
  echo "apply-pm-framework: extracted tree lacks CONFIG_PM_ENABLE=y" >&2; exit 1; }
grep -q "define CONFIG_SPIRAM_MODE_OCT 1" "$FRAMEWORK/tools/sdk/esp32s3/qio_opi/include/sdkconfig.h" || {
  echo "apply-pm-framework: qio_opi variant lost OPI PSRAM config" >&2; exit 1; }

size_after=$(pm_size)
[ "$size_after" -gt 100 ] || {
  echo "apply-pm-framework: esp_pm_configure still a stub (${size_after}B) after overlay" >&2
  exit 1
}
echo "apply-pm-framework: OK — esp_pm_configure ${size_before}B -> ${size_after}B"
