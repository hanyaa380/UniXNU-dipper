/* SPDX-License-Identifier: MIT */
#include "kernel/QcomUsbScreenshot.h"
#include "kernel/QcomUsbControl.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static unsigned mode;
static int capture(void *cookie, uint8_t *rgb, size_t capacity, uint32_t *w, uint32_t *h)
{
    assert(cookie == &mode && capacity >= 3000);
    if (mode == 2) return 0;
    *w = mode == 3 ? 0 : 1000; *h = 1;
    for (unsigned i = 0; i < 3000; ++i) rgb[i] = mode ? (uint8_t)i : (uint8_t)(i % 3);
    return 1;
}
int main(int argc, char **argv)
{
    struct qcom_usb_screenshot_info info;
    uint8_t data[192], again[192]; uint32_t offset, repeated;
    assert(!qcom_usb_shot_info(&info)); assert(qcom_usb_shot_capture(1) == -1);
    if (argc > 1 && argv[1][0] == 'f') {
        const uint8_t bgra[] = {3,2,1,255,6,5,4,255};
        struct qcom_usb_framebuffer fb = {bgra, sizeof(bgra), 2, 1, 8, QCOM_USB_BGRA8888};
        fb.bytes = 7; assert(!qcom_usb_debug_register_framebuffer(&fb));
        fb.bytes = 8; assert(qcom_usb_debug_register_framebuffer(&fb));
        assert(!qcom_usb_debug_register_framebuffer(&fb));
        assert(qcom_usb_shot_capture(8) == 1 && qcom_usb_shot_info(&info));
        assert(info.width == 2 && info.height == 1 && info.bytes == 6 && !info.rle);
        assert(qcom_usb_shot_peek(data, sizeof(data), &offset) == 6);
        assert(!memcmp(data, "\1\2\3\4\5\6", 6));
        puts("PASS fixed BGRA adapter, size validation and one-time registration"); return 0;
    }
    assert(!qcom_usb_debug_register_screenshot(0, 0));
    assert(qcom_usb_debug_register_screenshot(capture, &mode));
    assert(!qcom_usb_debug_register_screenshot(capture, &mode));
    for (mode = 0; mode < 2; ++mode) {
        uint32_t total = 0;
        assert(qcom_usb_shot_capture(9) == 1 && qcom_usb_shot_info(&info));
        assert(info.frame == 9 && info.bytes == 3000 && info.rle == !mode);
        for (;;) {
            size_t n = qcom_usb_shot_peek(data, sizeof(data), &offset);
            assert(offset == total);
            if (!n) break;
            assert(qcom_usb_shot_peek(again, sizeof(again), &repeated) == n);
            assert(offset == repeated && !memcmp(data, again, n));
            assert(!qcom_usb_shot_commit(n + 1));
            assert(qcom_usb_shot_commit(n)); assert(!qcom_usb_shot_commit(n));
            total += (uint32_t)n;
        }
        assert(total == info.encoded_bytes && total <= info.bytes);
    }
    mode = 2; assert(qcom_usb_shot_capture(10) == -2 && !qcom_usb_shot_info(&info));
    mode = 3; assert(qcom_usb_shot_capture(10) == -2 && !qcom_usb_shot_info(&info));
    mode = 0; assert(qcom_usb_shot_capture(11) == 1);
    qcom_usb_shot_cancel(); assert(!qcom_usb_shot_info(&info));
    puts("PASS snapshot raw/RLE, backpressure peek/commit, failure and cancellation");
    return 0;
}
