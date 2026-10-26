// FatFs's directory API over the sim card's host directory: drive "0:" is
// the card. Only what ride_recorder's fast directory scan uses.
#pragma once
#include <stdint.h>
#define FF_VOLUMES 1
#define AM_DIR 0x10
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_NO_PATH = 5, FR_INVALID_DRIVE = 11 } FRESULT;
typedef struct { void* d; } FF_DIR;
typedef struct { uint32_t fsize; uint16_t fdate, ftime; uint8_t fattrib; char altname[13]; char fname[256]; } FILINFO;
#ifdef __cplusplus
extern "C" {
#endif
FRESULT f_opendir(FF_DIR* dp, const char* path);
FRESULT f_readdir(FF_DIR* dp, FILINFO* fno);
FRESULT f_closedir(FF_DIR* dp);
#ifdef __cplusplus
}
#endif
