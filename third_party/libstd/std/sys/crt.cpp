#include "crt.h"

#include <std/dbg/insist.h>
#include <std/ios/output.h>

#include <time.h>
#include <stdlib.h>
#include <string.h>

void* stl::allocateMemory(size_t len) {
    if (auto ret = malloc(len); ret) {
        return ret;
    }

    STD_INSIST(false);

    return 0;
}

void stl::freeMemory(void* ptr) noexcept {
    free(ptr);
}

int stl::memCmp(const void* l, const void* r, size_t len) noexcept {
    if (len == 0) {
        return 0;
    }

    return memcmp(l, r, len);
}

void* stl::memCpy(void* to, const void* from, size_t len) noexcept {
    if (len) {
        memcpy(to, from, len);
    }

    return len + (u8*)to;
}

size_t stl::strLen(const u8* s) noexcept {
    return s ? strlen((const char*)s) : 0;
}

void stl::memZero(void* from, void* to) noexcept {
    const size_t len = (u8*)to - (u8*)from;

    if (len) {
        memset(from, 0, len);
    }
}

__attribute__((weak)) u64 stl::monotonicNowUs() noexcept {
    timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000;
}
