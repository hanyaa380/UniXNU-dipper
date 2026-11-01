/* SPDX-License-Identifier: MIT */
/* Built into BSD with its real compile flags. No GPU or IOKit dependency. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include "QcomUserLogWire.h"
#include "QcomXnuUsbDebug.h"

int
qcom_usb_userlog_submit(const void *input, size_t size)
{
    if (proc_suser(current_proc()) != 0) return EPERM;
    if (!input || size < offsetof(struct QcomUserLogWire, data) || size > sizeof(struct QcomUserLogWire))
        return EINVAL;
    const struct QcomUserLogWire *w = input;
    if (!qcom_ulog_valid(w, size)) return EINVAL;
    char record[2400];
    int n = snprintf(record, sizeof record,
        "\nQCOM_ULOG v=1 sid=%08x%08x stream=%u seq=%u total=%u off=%u bytes=%u crc=%08x flags=%u data=",
        w->sessionHi, w->sessionLo, w->stream, w->sequence, w->total, w->offset, w->bytes, w->crc, w->flags);
    if (n <= 0 || (size_t)n + w->bytes * 2u + 1u >= sizeof record) return EMSGSIZE;
    size_t length = (size_t)n;
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < w->bytes; ++i) {
        record[length++] = hex[w->data[i] >> 4]; record[length++] = hex[w->data[i] & 15u];
    }
    record[length++] = '\n';
    return qcom_usb_debug_try_record(record, length) == length ? 0 : EAGAIN;
}

static int
qcom_usb_userlog_sysctl SYSCTL_HANDLER_ARGS
{
    (void)oidp; (void)arg1; (void)arg2;
    if (proc_suser(current_proc()) != 0) return EPERM;
    if (req->oldptr || !req->newptr || req->newlen < offsetof(struct QcomUserLogWire, data) ||
        req->newlen > sizeof(struct QcomUserLogWire)) return EINVAL;
    struct QcomUserLogWire record = {0};
    int error = SYSCTL_IN(req, &record, req->newlen);
    return error ? error : qcom_usb_userlog_submit(&record, req->newlen);
}
SYSCTL_PROC(_debug, OID_AUTO, qcom_usb_userlog,
    CTLTYPE_OPAQUE | CTLFLAG_WR | CTLFLAG_LOCKED,
    0, 0, qcom_usb_userlog_sysctl, "S", "Root-only framed USB diagnostic log input");
