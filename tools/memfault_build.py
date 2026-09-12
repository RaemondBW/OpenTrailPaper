"""Build the pinned Memfault SDK against the existing Arduino/IDF 4.4 SDK.

No framework replacement, network task, or heartbeat timer. The ESP-IDF panic
and flash ports are upstream; our transport defers acknowledgement until SD
read-back verification. Run before elf2image so its ELF SHA also matches.
"""
from pathlib import Path
import subprocess
import hashlib
import re
from SCons.Script import DefaultEnvironment

Import("env")
root = Path(env.subst("$PROJECT_DIR"))
sdk = root / "third_party/memfault-firmware-sdk"
env.Append(CPPPATH=[str(root / "src"), str(sdk / "components/include"),
                    str(sdk / "ports/include"),
                    str(sdk / "ports/esp_idf/memfault/include")])
env.Append(CPPDEFINES=[("OT_MEMFAULT", 1)])
env.Append(LINKFLAGS=["-Wl,--wrap=" + name for name in (
    "esp_core_dump_init", "esp_core_dump_to_flash", "esp_core_dump_image_get",
    "panic_abort", "memfault_platform_coredump_storage_clear")])

sources = {
    "core": ["memfault_build_id.c", "memfault_core_utils.c", "memfault_data_export.c",
             "memfault_data_packetizer.c", "memfault_log.c",
             "memfault_ram_reboot_info_tracking.c", "memfault_sdk_assert.c",
             "memfault_event_storage.c", "memfault_reboot_tracking_serializer.c",
             "memfault_serializer_helper.c",
             "memfault_task_watchdog.c"],
    "panics": ["memfault_coredump.c", "memfault_coredump_sdk_regions.c",
               "memfault_coredump_utils.c", "memfault_fault_handling_xtensa.c"],
    "util": ["*.c"],
}
for component, files in sources.items():
    env.BuildSources("$BUILD_DIR/memfault/" + component,
                     str(sdk / "components" / component / "src"),
                     src_filter=" ".join("+<" + name + ">" for name in files))
env.BuildSources("$BUILD_DIR/memfault/esp_common",
                 str(sdk / "ports/esp_idf/memfault/common"))
env.BuildSources("$BUILD_DIR/memfault/esp_flash",
                 str(sdk / "ports/esp_idf/memfault/v4.x"))

# The SDK includes its config through a macro, which SCons' C scanner does
# not reliably follow. Without explicit edges an incremental build can retain
# objects compiled with old capture/identity settings.
config = env.File(str(root / "src/memfault_platform_config.h"))
env.Depends(DefaultEnvironment()["PIOBUILDFILES"], config)
for source in ("memfault_port", "memfault_service"):
    env.Depends("$BUILD_DIR/src/" + source + ".cpp.o", config)

def stamp_build_id(source, target, env):
    result = subprocess.run([env.subst("$PYTHONEXE"), "-B", str(sdk / "scripts/fw_build_id.py"),
                             str(target[0])], check=True, text=True, capture_output=True)
    print(result.stdout, end="")
    build_id = re.findall(r"[0-9a-f]{40}", result.stdout)[-1]
    data = Path(str(target[0])).read_bytes()
    # Rebuilding must not discard the symbols for firmware still on a bike.
    # Distinct debug sections may share a Memfault build ID, so keep the full
    # ELF hash too; it also matches the boot report's ELF SHA256.
    folder = root / ".pio/memfault-symbols" / build_id
    folder.mkdir(parents=True, exist_ok=True)
    archive = folder / (hashlib.sha256(data).hexdigest() + ".elf")
    if archive.exists():
        if archive.read_bytes() != data:
            raise RuntimeError("Existing Memfault symbol archive is corrupt: " + str(archive))
    else:
        with archive.open("xb") as output:
            output.write(data)
    print("Memfault symbols preserved: " + str(archive))

env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", stamp_build_id)
