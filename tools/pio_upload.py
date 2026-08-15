"""PlatformIO hooks that make `pio run -t upload` hands-free.

PlatformIO still does the flashing -- it knows the image list, offsets and flash
settings, and we do not want to reimplement that here. This script only wraps
the two ends that the board's USB-OTG mode breaks:

  before upload  reboot the running firmware into the ROM loader (if it isn't
                 there already) and point UPLOAD_PORT at the download-mode port,
                 which is a *different* device from the app's console port.

  after upload   clear RTC_CNTL_FORCE_DOWNLOAD_BOOT and reset with GPIO0 high,
                 so the board boots the image we just wrote instead of sitting
                 in download mode waiting for a RESET tap.

The `--before no_reset --after no_reset` that this depends on is not set here:
it comes from board_upload.before_reset / board_upload.after_reset in
platformio.ini, so the flags stay visible next to the rest of the upload config.

The reasoning behind each step is in tools/flash.py, which does the real work.
"""

import os
import sys

Import("env")  # noqa: F821 - injected by SCons

# SCons exec()s this file without setting __file__, so locate our sibling module
# through the project root instead.
sys.path.insert(0, os.path.join(env["PROJECT_DIR"], "tools"))  # noqa: F821
import flash  # noqa: E402


def before_upload(source, target, env):
    port = flash.enter_download_mode()
    env.Replace(UPLOAD_PORT=port)


def after_upload(source, target, env):
    # Re-detect rather than reusing the port from before_upload: esptool has
    # closed it by now, and on a re-enumeration the node could differ.
    port = env.subst("$UPLOAD_PORT")
    if not os.path.exists(port):
        port = flash.find_rom_port()
    if not port:
        print("[flash] board left download mode on its own; nothing to reset")
        return
    flash.finish(port)


env.AddPreAction("upload", before_upload)    # noqa: F821
env.AddPostAction("upload", after_upload)    # noqa: F821
