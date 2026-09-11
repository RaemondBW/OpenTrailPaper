#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Caller supplies storage and synchronization. Keep complete recent lines on
// overflow and acknowledge only bytes accepted by the storage writer.
class DiagBuffer {
public:
    DiagBuffer(char* storage, size_t capacity) : data(storage), capacity(capacity) {}
    void append(const char* text, size_t count) {
        if (!data || !capacity) return;
        if (count > capacity) { dropped += count; return; }
        if (size + count > capacity) {
            size_t remove = size + count - capacity;
            while (remove < size && data[remove - 1] != '\n') ++remove;
            dropped += remove;
            consume(remove);
        }
        memcpy(data + size, text, count);
        size += count;
    }
    void consume(size_t count) {
        if (count > size) count = size;
        memmove(data, data + count, size - count);
        size -= count;
    }
    char* data;
    size_t capacity;
    size_t size = 0;
    uint32_t dropped = 0;
};
