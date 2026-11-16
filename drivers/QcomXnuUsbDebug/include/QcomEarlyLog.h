/* SPDX-License-Identifier: MIT */
#ifndef QCOM_EARLY_LOG_H
#define QCOM_EARLY_LOG_H
#include <stdint.h>

/* Written through a PC-relative physical address by start_first_cpu before
 * bootstrap mappings. Consumed only after arm_slide_rebase_and_sign_image. */
extern uint64_t qcom_early_entry_ticks;
void qcom_early_log_stage(const char *stage);

/* No CPU data, allocation, formatter locks, platform clock or USB hardware.
 * Keep raw counter/entry/frequency alongside delta_us: the inherited timer
 * epoch is not kernel uptime. Invalid readings stay explicitly unqualified. */
static inline void
qcom_early_text(const char *text, void (*putc)(char))
{
    while (*text) putc(*text++);
}

static inline void
qcom_early_uint(uint64_t value, void (*putc)(char))
{
    char digits[20];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) putc(digits[--count]);
}

static inline void
qcom_early_emit(const char *stage, uint64_t now, uint64_t entry,
                    uint64_t frequency, void (*putc)(char))
{
    unsigned length = 0;
    qcom_early_text("QCOM EARLY stage=", putc);
    while (stage[length] && length < 80) putc(stage[length++]);
    if (stage[length]) qcom_early_text("[truncated]", putc);
    qcom_early_text(" counter_ticks=", putc);
    qcom_early_uint(now, putc);
    qcom_early_text(" entry_ticks=", putc);
    qcom_early_uint(entry, putc);
    qcom_early_text(" freq_hz=", putc);
    qcom_early_uint(frequency, putc);
    qcom_early_text(" delta_us=", putc);
    if (!frequency || frequency > UINT64_C(1000000000) ||
        entry == UINT64_MAX || now < entry ||
        (now - entry) / frequency > UINT64_MAX / UINT64_C(1000000)) {
        qcom_early_text("invalid", putc);
    } else {
        uint64_t delta = now - entry;
        uint64_t whole = (delta / frequency) * UINT64_C(1000000);
        uint64_t fraction = (delta % frequency) * UINT64_C(1000000) / frequency;
        if (fraction > UINT64_MAX - whole) qcom_early_text("invalid", putc);
        else qcom_early_uint(whole + fraction, putc);
    }
    putc('\n');
}
#endif
