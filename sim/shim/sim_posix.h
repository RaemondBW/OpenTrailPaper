// Force-included into the firmware sources (sim/build.sh): POSIX calls on the
// SD card's VFS mount ("/sd/...") reach the sim's card, as they reach FatFs
// through ESP-IDF's VFS on the board. Everything else passes through to the
// host (fs_sim.cpp).
#pragma once
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif
DIR* sim_opendir(const char* path);
int sim_stat(const char* path, struct stat* st);
FILE* sim_fopen(const char* path, const char* mode);
#ifdef __cplusplus
}
#endif

#define opendir(p) sim_opendir(p)
#define stat(p, st) sim_stat(p, st)
#define fopen(p, m) sim_fopen(p, m)
