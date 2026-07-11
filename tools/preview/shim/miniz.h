// Dummy tinfl API — our fonts are uncompressed, so the decompression path
// in epdiy's font.c never executes on host.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { int dummy; } tinfl_decompressor;
typedef int tinfl_status;
#define TINFL_STATUS_DONE 0
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 2
#define tinfl_init(r) do { (void)(r); } while (0)

static inline tinfl_status tinfl_decompress(
    tinfl_decompressor* d, const uint8_t* in, size_t* in_size,
    uint8_t* out_start, uint8_t* out, size_t* out_size, int flags) {
    (void)d; (void)in; (void)in_size; (void)out_start; (void)out;
    (void)out_size; (void)flags;
    return -1;  // fail loudly if a compressed font sneaks in
}
