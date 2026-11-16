/* SPDX-License-Identifier: MIT
 * Executes the production control handler extracted by tests/check.py.
 */
#include "../include/QcomXnuUsbDebug.h"
#include "kernel/QcomUsbScreenshot.h"
#include "kernel/QcomUsbControl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct { struct sxnu_control_state state; } usb_control_context;
static unsigned control_hook_owned, control_reboot_requested, control_reboot_taken;
static unsigned control_reboot_armed, control_reboot_seq, control_reboot_generation;
static uint64_t control_reboot_session;
static unsigned control_rejected, blocked, captures, fail_capture;
static size_t control_reply_queue(const char *reply, size_t bytes)
{
    if (blocked) return 0;
    assert(bytes <= SXNU_CONTROL_REPLY_BYTES);
    assert(fwrite(reply, 1, bytes, stdout) == bytes);
    return bytes;
}
static size_t control_reply_hello(char *p, size_t n, const struct sxnu_control_frame *f)
{ (void)p; (void)n; (void)f; return 0; }
static size_t control_reply_ping(char *p, size_t n, const struct sxnu_control_frame *f,
    const struct qcom_usb_debug_status *s)
{ (void)p; (void)n; (void)f; (void)s; return 0; }
static size_t control_reply_status(char *p, size_t n, const struct sxnu_control_frame *f,
    const struct qcom_usb_debug_status *s)
{ return control_reply_ping(p, n, f, s); }
#include "control-handler.inc"
static int capture(void *cookie, uint8_t *rgb, size_t capacity, uint32_t *w, uint32_t *h)
{
    (void)cookie; assert(capacity >= 24); ++captures;
    if (fail_capture) return 0;
    *w = 4; *h = 2;
    for (unsigned i = 0; i < 24; ++i) rgb[i] = (uint8_t)(i * 7);
    return 1;
}
static void send_request(enum sxnu_control_verb verb, uint32_t seq)
{
    const struct sxnu_control_frame frame = {7, seq, 0, verb};
    struct qcom_usb_debug_status status = {0};
    control_handle_frame(&frame, &status, 1);
}
int main(void)
{
    sxnu_control_state_reset(&usb_control_context.state, 7, 1);
    assert(qcom_usb_debug_register_screenshot(capture, 0));
    blocked = 1; send_request(SXNU_CONTROL_VERB_SCREENSHOT, 1);
    assert(usb_control_context.state.last_seq == 0);
    blocked = 0; send_request(SXNU_CONTROL_VERB_SCREENSHOT, 1);
    assert(captures == 2 && usb_control_context.state.last_seq == 1);
    blocked = 1; send_request(SXNU_CONTROL_VERB_SHOTNEXT, 2);
    assert(usb_control_context.state.last_seq == 1);
    blocked = 0; send_request(SXNU_CONTROL_VERB_SHOTNEXT, 2);
    assert(usb_control_context.state.last_seq == 2);
    unsigned rejected = control_rejected;
    send_request(SXNU_CONTROL_VERB_SHOTNEXT, 2); assert(control_rejected == rejected + 1);
    fail_capture = 1; send_request(SXNU_CONTROL_VERB_SCREENSHOT, 3);
    send_request(SXNU_CONTROL_VERB_SHOTNEXT, 4);
    assert(usb_control_context.state.last_seq == 4);
    send_request(SXNU_CONTROL_VERB_REBOOT, 5); /* no hook: must not queue reset */
    assert(!control_reboot_requested);
    control_hook_owned = 1; send_request(SXNU_CONTROL_VERB_REBOOT, 6);
    assert(control_reboot_requested && control_reboot_session == 7 && control_reboot_seq == 6);
    return 0;
}
