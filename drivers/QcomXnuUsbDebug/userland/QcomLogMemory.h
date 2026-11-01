/* SPDX-License-Identifier: MIT */
#ifndef QCOM_LOG_MEMORY_H
#define QCOM_LOG_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>

namespace QcomUserLog {
// Exact Darwin22 bsd/sys/kern_memorystatus.h v1 ABI, commands 7 and 8.
struct LogMemoryLimits {
    int32_t active;
    uint32_t activeAttributes;
    int32_t inactive;
    uint32_t inactiveAttributes;
};
static_assert(sizeof(LogMemoryLimits) == 16, "Darwin22 memory-limit ABI");

static bool configureIos16MemoryBudget(char *message, size_t capacity) {
    using Control = int (*)(uint32_t, int32_t, uint32_t, void *, size_t);
    LogMemoryLimits before{}, after{}, requested{128, 1, 128, 1};
    const char *stage = "root";
    int error = EPERM;
    bool ok = false;
    if (geteuid() == 0) {
        stage = "symbol";
        error = ENOSYS;
        auto control = reinterpret_cast<Control>(dlsym(RTLD_DEFAULT, "memorystatus_control"));
        if (control) {
            // Only this diagnostic process; no global limits, priorities or
            // entitlement/MAC checks are changed. Native denial stays a failure.
            const int32_t pid = getpid();
            stage = "before";
            if (control(8, pid, 0, &before, sizeof before) == 0) {
                stage = "set";
                if (control(7, pid, 0, &requested, sizeof requested) == 0) {
                    stage = "readback";
                    if (control(8, pid, 0, &after, sizeof after) == 0) {
                        ok = after.active == 128 && after.inactive == 128 &&
                             after.activeAttributes == 1 && after.inactiveAttributes == 1;
                        error = ok ? 0 : EIO;
                        stage = ok ? "verified" : "mismatch";
                    } else error = errno;
                } else error = errno;
            } else error = errno;
        }
    }
    snprintf(message, capacity,
             "collector_memory_budget stage=%s error=%d active_mb=%d active_attr=%u "
             "inactive_mb=%d inactive_attr=%u previous_active_mb=%d previous_inactive_mb=%d",
             stage, error, after.active, after.activeAttributes, after.inactive,
             after.inactiveAttributes, before.active, before.inactive);
    return ok;
}
}
#endif
