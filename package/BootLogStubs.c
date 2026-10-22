// SPDX-License-Identifier: BSD-2-Clause-Patent
// Weak stand-ins for the diagnostics package/BootLog.c reads (weak_import) from kits
// the fajita kernel does not link yet: sheng's display, HID, Adreno, Wi-Fi, touch and
// button kits. Apple ld64 refuses an *undefined* weak_import symbol in the static
// kernel link, so each referenced symbol gets a weak definition here, exactly as
// package/BootProgressStubs.c does for BootProgress.c. The data reads as zero and the
// functions report nothing; a real definition, once a kit is linked, takes precedence
// over these weak ones. The symbol set is the undefined-symbol list of the fajita
// kernel link, kept in step with BootLog.c's weak_import declarations.
#include <stdint.h>

#define WEAK __attribute__((weak))

// Structs BootLog.c defines for its Adreno client/asker rows.
struct native_adreno_client_record { char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal; };
struct native_adreno_asker_record  { char name[20]; uint32_t reads; };

// --- scalars ---
WEAK volatile uint64_t native_adreno_fence_phys;
WEAK volatile uint32_t native_adreno_ib_log_count;
WEAK volatile uint64_t native_adreno_import_fail_pa;
WEAK volatile uint64_t native_adreno_import_fail_offset;
WEAK volatile uint32_t native_adreno_import_stage;
WEAK volatile uint32_t native_adreno_io_coherent;
WEAK volatile uint32_t native_adreno_pool_status;
WEAK volatile uint32_t native_adreno_pool_wc;
WEAK volatile uint32_t native_adreno_ring_log_count;
WEAK volatile uint64_t native_gpu_idle_sleeps;
WEAK volatile uint64_t native_gpu_idle_spins;
WEAK volatile uint32_t native_touch_stroke_count;

// --- arrays ---
WEAK volatile uint32_t native_adreno_coherent_skips[4];
WEAK volatile uint32_t native_adreno_export_stats[4];
WEAK volatile uint32_t native_adreno_fence_wait[8];
WEAK volatile uint32_t native_adreno_ib_log[4][4 + 28];
WEAK volatile uint64_t native_adreno_import_diag[10];
WEAK volatile uint32_t native_adreno_map_fail[7][2];
WEAK volatile uint32_t native_adreno_method_stats[20][4];
WEAK volatile uint32_t native_adreno_mount_fail[4];
WEAK volatile uint32_t native_adreno_pool_stats[8];
WEAK volatile uint32_t native_adreno_ring_log[4][4 + 112];
WEAK volatile uint32_t native_adreno_view_diag[6];
WEAK volatile uint64_t native_adreno_vm_phase_us[2][8];
WEAK volatile uint64_t native_adreno_wait_outer[4];
WEAK volatile uint32_t native_adreno_window_overflow[4][3];
WEAK volatile int32_t  native_backlight_stats[10];
WEAK volatile int32_t  native_buttons_stats[10];
WEAK volatile uint64_t native_gop_wait_stats[7];
WEAK char              native_gpu_coherence_row[120];
WEAK uint32_t          native_gpu_perf_vote[2];
WEAK volatile uint32_t native_swipe_stats[6];
WEAK volatile uint32_t native_touch_edge_stats[4];
WEAK volatile int32_t  native_touch_extent[4];
WEAK volatile int32_t  native_touch_strokes[16][7];
WEAK volatile uint32_t native_wifi_stats[10];
WEAK char              native_adreno_metal_rows[48][96];

// --- structured arrays ---
WEAK struct native_adreno_client_record native_adreno_client_log[8];
WEAK struct native_adreno_asker_record  native_adreno_metal_askers[12];
// native_adreno_client_text: {char name[20]; char text[64];} per BootLog.c.
struct native_adreno_client_text { char name[20]; char text[64]; };
WEAK struct native_adreno_client_text native_adreno_client_text[4];

// --- function pointers (null: BootLog.c guards them with &ptr && ptr) ---
WEAK int (*native_adreno_dram_read32)(uint64_t, uint32_t *);
WEAK int (*native_adreno_cached_read32)(uint64_t, uint32_t *);

// --- functions (report nothing) ---
WEAK unsigned boot_ui_trace_hot_rows(char rows[][120], unsigned cap) { (void)rows; (void)cap; return 0; }
WEAK unsigned native_legacy_clients_dump(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
WEAK unsigned native_legacy_prepare_report(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
WEAK unsigned native_legacy_properties_now(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
WEAK unsigned native_legacy_state_words(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
WEAK unsigned native_legacy_swap_timeline(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
