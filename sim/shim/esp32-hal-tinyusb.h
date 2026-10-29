#pragma once
typedef enum { RESTART_NO_PERSIST, RESTART_PERSIST, RESTART_BOOTLOADER, RESTART_BOOTLOADER_DFU, RESTART_TYPE_MAX } restart_type_t;
void usb_persist_restart(restart_type_t mode);
