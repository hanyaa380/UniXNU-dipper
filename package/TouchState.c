// SPDX-License-Identifier: BSD-2-Clause-Patent
// The touch snapshot NativeHidInput reads (drivers/NativeHidInput/NativeHidInput.cpp: a seqlock of contact count, identifiers and
// coordinates, written by the board's touch driver). sheng's TouchProbe.cpp defines it for its Novatek panel; fajita has no touch
// driver yet (Synaptics S3706 over GENI I2C), so nothing writes it: the sequence stays 0, NativeHidInput's touchSend() never
// builds a digitizer report, and NativeHidInput only publishes its keyboard (Home) collection unless sheng-touch-hid=1.
// Remove this file once a fajita TouchProbe defines the same symbols.
#include <stdint.h>
#include <stddef.h>

volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];

// NativeHidInput registers its report callback here (weak_import, but the static kernel link still needs a definition). The touch
// driver would call it after each snapshot; without one it is only kept.
static void (*touch_sink)(void *);
static void *touch_sink_context;
void native_touch_set_sink(void (*sink)(void *), void *context);
void native_touch_set_sink(void (*sink)(void *), void *context)
{
  touch_sink_context = context;
  __atomic_store_n(&touch_sink, sink, __ATOMIC_RELEASE);
}
