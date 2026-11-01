// SPDX-License-Identifier: MIT
// Adapted from NativeMetal NativeOsLogExport.mm (88849ec); see LICENSE.NativeMetal.
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <objc/message.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <zlib.h>
#include <string>
#include <os/log.h>
#include <stdlib.h>
#include <mach/mach_time.h>
#include "QcomUserLogTransport.h"
#include "QcomIos16LogStore.h"
#include "QcomLogMemory.h"

namespace {

constexpr const char *kDirectory = "/private/var/tmp/qcom-oslog";
constexpr const char *kLock = "/private/var/tmp/qcom-oslog.lock";
constexpr unsigned kPasses = 40, kPeriodSeconds = 45;
constexpr uint64_t kLineLimit = 60000;

const char *levelName(NSInteger level) {
    switch (level) {
    case 1: return "debug";
    case 2: return "info";
    case 3: return "notice";
    case 4: return "error";
    case 5: return "fault";
    default: return "-";
    }
}

#include "QcomOsLogPages.inc"

}  // namespace

// Runs the passes (blocks for kPasses * kPeriodSeconds); returns at once if another instance holds the lock or the cache directory is absent.
extern "C" void qcomOsLogExport() {
    const int lock = open(kLock, O_CREAT | O_RDWR, 0600);
    if (lock < 0) { QcomUserLog::status("lock_open_failed"); return; }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) { close(lock); return; }
    uint64_t canaryNonce; arc4random_buf(&canaryNonce, sizeof canaryNonce);
    canaryNonce ^= mach_absolute_time();
    char started[160];
    snprintf(started, sizeof started, "collector_started version=2 upstream=88849ec system_scope=0 canary_nonce=%016llx", (unsigned long long)canaryNonce);
    QcomUserLog::status(started);
    char budget[320];
    bool memoryReady = QcomUserLog::configureIos16MemoryBudget(budget, sizeof budget);
    QcomUserLog::status(budget);
    if (!memoryReady) { close(lock); return; }
    os_log_error(OS_LOG_DEFAULT, "QCOM USERLOG CANARY pid=%{public}d nonce=%{public}016llx", getpid(), (unsigned long long)canaryNonce);
    for (unsigned pass = 1; pass <= kPasses; ++pass) {
        mkdir(kDirectory, 0755);
        struct stat st = {};
        if (stat(kDirectory, &st) == 0 && S_ISDIR(st.st_mode)) writePass(pass);
        else QcomUserLog::status("snapshot_directory_missing");
        sleep(kPeriodSeconds);
    }
    close(lock);
}
