/* SPDX-License-Identifier: MIT */
/* CPU-only model: exercises the actual native backlog header, not a USB or
 * device assertion. Native callers additionally preserve DAIF around guards. */
#include "kernel/QcomUsbBuffer.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static uint8_t storage[SOC_USB_LOG_BUFFER_BYTES], received[SOC_USB_LOG_BUFFER_BYTES];
static struct soc_usb_buffer ring;
static size_t received_bytes, sink_limit;
static void reset(size_t capacity)
{
    memset(&ring, 0, sizeof(ring));
    ring.data = storage; ring.capacity = capacity;
    received_bytes = 0; sink_limit = sizeof(received);
}
static size_t sink(void *cookie, const void *bytes, size_t count)
{
    (void)cookie;
    if (count > sink_limit) count = sink_limit;
    assert(received_bytes + count <= sizeof(received));
    memcpy(received + received_bytes, bytes, count); received_bytes += count;
    return count;
}
static void drain(void)
{
    while (soc_usb_buffer_pending(&ring))
        assert(soc_usb_buffer_drain(&ring, sink, 0, SOC_USB_LOG_DRAIN_BUDGET));
}
static void early_and_partial(void)
{
    uint8_t bytes[60];
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)i;
    reset(31);
    assert(soc_usb_buffer_write(&ring, bytes, sizeof(bytes)) == 31);
    assert(ring.high_water == 31 && ring.overflow == 29 && ring.contended == 0);
    sink_limit = 0;
    assert(soc_usb_buffer_drain(&ring, sink, 0, 30) == 0);
    assert(soc_usb_buffer_pending(&ring) == 31 && ring.tail == 0);
    sink_limit = 7;
    assert(soc_usb_buffer_drain(&ring, sink, 0, 30) == 7);
    assert(soc_usb_buffer_write(&ring, bytes + 31, 7) == 7);
    drain();
    assert(received_bytes == 38 && !memcmp(received, bytes, received_bytes));
    assert(ring.accepted == 38 && soc_usb_buffer_dropped(&ring) == 29);
    puts("PASS: early oldest bytes survive overflow; zero/partial sink acceptance and wrap replay preserve exact order");
}
static size_t recursive_sink(void *cookie, const void *bytes, size_t count)
{
    assert(soc_usb_buffer_write(&ring, "nested", 6) == 0);
    return sink(cookie, bytes, count);
}
static void nonspinning(void)
{
    reset(sizeof(storage));
    assert(soc_usb_buffer_write(&ring, "original", 8) == 8);
    assert(soc_usb_buffer_drain(&ring, recursive_sink, 0, 8) == 8);
    assert(ring.contended == 6 && ring.overflow == 0 && !memcmp(received, "original", 8));
    assert(soc_usb_buffer_pending(&ring) == 0);
    ring.busy = 1;
    assert(soc_usb_buffer_write(&ring, "IRQ", 3) == 0);
    assert(soc_usb_buffer_drain(&ring, sink, 0, 3) == 0);
    ring.busy = 0;
    assert(ring.contended == 9);
    puts("PASS: recursive and competing producers return immediately and count every rejected byte");
}
static void large_backlog(void)
{
    uint8_t chunk[SOC_USB_LOG_WRITE_BUDGET];
    reset(sizeof(storage));
    for (size_t offset = 0; offset < sizeof(storage); offset += sizeof(chunk)) {
        for (size_t i = 0; i < sizeof(chunk); i++) chunk[i] = (uint8_t)((offset + i) * 17u);
        assert(soc_usb_buffer_write(&ring, chunk, sizeof(chunk)) == sizeof(chunk));
    }
    assert(ring.used == 4u * 1024u * 1024u && ring.high_water == sizeof(storage));
    assert(soc_usb_buffer_write(&ring, "overflow", 8) == 0);
    sink_limit = 127;
    drain();
    assert(received_bytes == sizeof(storage));
    for (size_t i = 0; i < received_bytes; i++) assert(received[i] == (uint8_t)(i * 17u));
    assert(ring.overflow == 8 && ring.accepted == sizeof(storage));
    puts("PASS: full 4 MiB backlog replays without overwrite under partial 127-byte USB-style acceptance");
}
#define PRODUCERS 4u
#define MESSAGES 10000u
static unsigned int started, finished;
static void *produce(void *argument)
{
    uint32_t id = (uint32_t)(uintptr_t)argument;
    while (!__atomic_load_n(&started, __ATOMIC_ACQUIRE)) {}
    for (uint32_t i = 0; i < MESSAGES; i++) {
        uint32_t message[2] = {id, i};
        size_t n = soc_usb_buffer_write(&ring, message, sizeof(message));
        assert(n == 0 || n == sizeof(message));
    }
    __atomic_fetch_add(&finished, 1u, __ATOMIC_RELEASE);
    return 0;
}
static void multiple_cpus(void)
{
    pthread_t workers[PRODUCERS];
    uint32_t previous[PRODUCERS] = {0}, seen[PRODUCERS] = {0};
    reset(sizeof(storage)); started = finished = 0;
    for (uintptr_t i = 0; i < PRODUCERS; i++) assert(!pthread_create(&workers[i], 0, produce, (void *)i));
    __atomic_store_n(&started, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) != PRODUCERS)
        (void)soc_usb_buffer_drain(&ring, sink, 0, SOC_USB_LOG_DRAIN_BUDGET);
    for (unsigned i = 0; i < PRODUCERS; i++) assert(!pthread_join(workers[i], 0));
    drain();
    assert(ring.overflow == 0 && ring.accepted == received_bytes && received_bytes % 8u == 0);
    assert(ring.accepted + ring.contended == PRODUCERS * MESSAGES * 8u);
    for (size_t i = 0; i < received_bytes; i += 8) {
        uint32_t message[2]; memcpy(message, received + i, sizeof(message));
        assert(message[0] < PRODUCERS && message[1] < MESSAGES);
        if (seen[message[0]]) assert(message[1] > previous[message[0]]);
        seen[message[0]]++; previous[message[0]] = message[1];
    }
    puts("PASS: four concurrent CPU-style producers and drain preserve complete ordered records and exact loss accounting");
}
int main(void)
{
    early_and_partial(); nonspinning(); large_backlog(); multiple_cpus();
    return 0;
}
