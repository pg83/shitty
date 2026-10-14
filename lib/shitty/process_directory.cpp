/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "process_directory.h"

#include <string.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#endif

using namespace stl;

bool processDirectory(pid_t pid, Buffer& out) {
    out.reset();
    if (pid <= 0) {
        return false;
    }
#if defined(__APPLE__)
    struct proc_vnodepathinfo info;
    // Poisoned rather than left as it came off the stack, so a partial
    // answer can never be mistaken for a whole one. proc_pidinfo reports
    // failure by returning 0, not a negative, so a caller checking only
    // for negatives reads whatever was in this buffer - which on a fresh
    // stack page is zeroes, reads as a plain empty path, and is
    // indistinguishable from an honest refusal. Filled with a byte that
    // is not a terminator it is distinguishable, which is what makes the
    // short-read check below a check a test can show the need for.
    memset(&info, 0xFF, sizeof(info));
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &info, sizeof(info)) != (int)(sizeof(info))) {
        return false;
    }
    const size_t length = strnlen(info.pvi_cdir.vip_path, sizeof(info.pvi_cdir.vip_path));
    if (length == 0) {
        return false;
    }
    out.append(info.pvi_cdir.vip_path, length);
    return true;
#elif defined(__linux__)
    char link[64];
    snprintf(link, sizeof(link), "/proc/%ld/cwd", (long)(pid));
    char path[PATH_MAX];
    // readlink does not terminate, and a path exactly PATH_MAX long
    // would be cut short without a sign - so one byte is held back and
    // a full buffer is refused rather than passed on truncated.
    const ssize_t length = readlink(link, path, sizeof(path) - 1);
    if (length <= 0 || (size_t)(length) >= sizeof(path) - 1) {
        return false;
    }
    out.append(path, (size_t)(length));
    return true;
#else
    return false;
#endif
}

bool processCommandLine(pid_t pid, Buffer& out) {
    out.reset();
    if (pid <= 0) {
        return false;
    }
#if defined(__APPLE__)
    // KERN_PROCARGS2: argc as an int, the executable's path, NUL padding,
    // then argc NUL-terminated arguments, then the environment - which is
    // why the count is read and not the whole buffer taken.
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, (int)(pid)};
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) < 0 || size <= sizeof(int)) {
        return false;
    }
    Buffer raw;
    raw.grow(size);
    if (sysctl(mib, 3, raw.mutData(), &size, nullptr, 0) < 0 || size <= sizeof(int)) {
        return false;
    }
    const char* const data = (const char*)(raw.mutData());
    int argc = 0;
    memcpy(&argc, data, sizeof(argc));
    size_t at = sizeof(argc);
    while (at < size && data[at] != '\0') {
        ++at;
    }
    while (at < size && data[at] == '\0') {
        ++at;
    }
    for (int index = 0; index < argc && at < size; ++index) {
        const size_t length = strnlen(data + at, size - at);
        if (at + length >= size) {
            // An argument with no terminator inside the buffer is a
            // truncated answer, not a command line.
            out.reset();
            return false;
        }
        out.append(data + at, length + 1);
        at += length + 1;
    }
    return out.used() != 0;
#elif defined(__linux__)
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)(pid));
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char chunk[4096];
    for (;;) {
        const ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got <= 0) {
            break;
        }
        out.append(chunk, (size_t)(got));
    }
    close(fd);
    if (out.used() == 0) {
        return false;
    }
    if (((const char*)(out.data()))[out.used() - 1] != '\0') {
        out.append("", 1);
    }
    return true;
#else
    return false;
#endif
}
