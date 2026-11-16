// SPDX-License-Identifier: MIT
#ifndef QCOM_USER_LOG_TRANSPORT_H
#define QCOM_USER_LOG_TRANSPORT_H
#include "QcomUserLogWire.h"
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

namespace QcomUserLog {
static unsigned stream;
static uint64_t session;
static uint64_t retries, failed;
static bool send(QcomUserLogWire &w)
{
    if (!session) { arc4random_buf(&session, sizeof session); if (!session) session = 1; }
    w.magic = SOC_ULOG_MAGIC; w.version = 1;
    w.sessionHi = (uint32_t)(session >> 32); w.sessionLo = (uint32_t)session;
    w.crc = qcom_ulog_crc(w.data, w.bytes);
    // Retain the exact frame across retryable full/contended FIFO responses.
    // Pace at <=8 KiB/s of compressed payload (hex wire roughly twice that).
    for (unsigned i = 0; i < 100; ++i) {
        int result = sysctlbyname("debug.qcom_usb_userlog", nullptr, nullptr, &w,
            offsetof(QcomUserLogWire, data) + w.bytes);
        if (!result) { usleep(125000); return true; }
        // Only retry a definite atomic rejection. Unknown outcomes are not replayed.
        if (errno != EAGAIN) { ++failed; return false; }
        ++retries; usleep(100000);
    }
    ++failed;
    return false;
}
static bool status(const char *text)
{
    QcomUserLogWire w{}; w.stream = ++stream; w.flags = SOC_ULOG_STATUS;
    int n = snprintf((char *)w.data, sizeof w.data, "%s retries=%llu failed=%llu", text,
                     (unsigned long long)retries, (unsigned long long)failed);
    if (n <= 0 || (size_t)n >= sizeof w.data) return false;
    w.total = w.bytes = (uint32_t)n;
    return send(w);
}
static bool snapshot(const char *path, unsigned pass)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0 || st.st_size > SOC_ULOG_MAX) {
        if (fd >= 0) close(fd);
        status("snapshot_open_or_size_failed"); return false;
    }
    QcomUserLogWire w{}; w.stream = ++stream; w.flags = SOC_ULOG_GZIP;
    w.total = (uint32_t)st.st_size;
    while (w.offset < w.total) {
        w.bytes = w.total - w.offset;
        if (w.bytes > SOC_ULOG_CHUNK) w.bytes = SOC_ULOG_CHUNK;
        size_t got = 0;
        while (got < w.bytes) {
            ssize_t n = read(fd, w.data + got, w.bytes - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (got != w.bytes || !send(w)) break;
        w.offset += w.bytes; ++w.sequence;
    }
    close(fd);
    char text[160];
    snprintf(text, sizeof text, "snapshot pass=%u stream=%u sent=%u total=%u complete=%u",
             pass, w.stream, w.offset, w.total, w.offset == w.total);
    bool reported = status(text);
    return w.offset == w.total && reported;
}
}
#endif
