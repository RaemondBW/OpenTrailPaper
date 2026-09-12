#!/usr/bin/env python3
"""Fingerprint actual framework content without editing PlatformIO packages."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core-dir', type=Path, default=Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio')))
    args = parser.parse_args()
    framework = args.core_dir / 'packages' / 'framework-arduinoespressif32'
    sdk = framework / 'tools' / 'sdk' / 'esp32s3'
    files = [framework / '.piopm', framework / 'tools' / 'platformio-build-esp32s3.py',
             framework / 'libraries' / 'SD' / 'src' / 'sd_diskio.cpp',
             framework / 'cores' / 'esp32' / 'esp32-hal-spi.c',
             sdk / 'sdkconfig', sdk / 'qio_opi' / 'include' / 'sdkconfig.h',
             sdk / 'lib' / 'libesp_pm.a', sdk / 'qio_opi' / 'libfreertos.a',
             sdk / 'lib' / 'libdriver.a', sdk / 'qio_opi' / 'libesp_hw_support.a']
    header = (sdk / 'qio_opi' / 'include' / 'sdkconfig.h').read_text()
    symbols = subprocess.check_output([
        str(args.core_dir / 'packages' / 'toolchain-xtensa-esp32s3' / 'bin' / 'xtensa-esp32s3-elf-nm'),
        '-S', '--defined-only', str(sdk / 'lib' / 'libesp_pm.a')], text=True)
    size = next((int(line.split()[1], 16) for line in symbols.splitlines()
                 if line.split() and line.split()[-1] == 'esp_pm_configure'), None)
    report = {
        'framework': str(framework.resolve()),
        'package': json.loads((framework / '.piopm').read_text()),
        'esp_pm_configure_bytes': size,
        'pm_configure_is_stub': size is not None and size <= 16,
        'config': [line for line in header.splitlines()
                   if re.search(r'CONFIG_(PM_|FREERTOS_USE_TICKLESS|BT_CTRL_(MODEM_SLEEP|LPCLK)|ESP32S3_RTC_CLK|SPIRAM_SPEED)', line)],
        'sha256': {str(p.relative_to(framework)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in files},
    }
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
