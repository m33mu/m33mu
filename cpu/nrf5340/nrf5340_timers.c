/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "nrf5340/nrf5340_timers.h"
#include "nrf5340/nrf5340_mmio.h"
#include "nrf5340/nrf5340_wdt.h"
#include "nrf_timers.h"

static struct nrf_timers_state g_timers;

void mm_nrf5340_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases_ns[] = { 0x4000F000u, 0x40010000u, 0x40011000u };
    static const mm_u32 bases_s[] = { 0x5000F000u, 0x50010000u, 0x50011000u };
    static const int irqs[] = { 15, 16, 17 };
    if (bus == 0) return;
    mm_nrf5340_wdt_set_nvic(nvic);
    mm_nrf5340_set_nvic(nvic);
    nrf_timers_register(&g_timers, bus, nvic, bases_ns, bases_s, irqs, 3u);
}

void mm_nrf5340_timers_reset(void)
{
    nrf_timers_reset(&g_timers);
}

void mm_nrf5340_timers_tick(mm_u64 cycles)
{
    size_t i;
    mm_nrf5340_rtc_tick(cycles);
    for (i = 0; i < g_timers.count; ++i) {
        struct nrf_timer_state *t = &g_timers.timers[i];
        mm_u64 ticks;
        mm_u64 div;
        mm_u32 prescaler;
        mm_u32 mask;
        mm_u32 old;
        mm_u32 now;
        mm_u32 ch;
        if (!t->running) continue;
        if (!mm_nrf5340_clock_hf_running()) continue;
        prescaler = t->regs[0x510u / 4u] & 0xFu;
        div = 1ull << prescaler;
        if (div == 0u) div = 1u;
        t->accum += cycles;
        ticks = t->accum / div;
        t->accum = t->accum % div;
        if (ticks == 0u) continue;
        mask = nrf_timer_bitmask(t);
        old = t->counter;
        now = (old + (mm_u32)ticks) & mask;
        t->counter = now;
        for (ch = 0; ch < t->cc_count; ++ch) {
            mm_u32 cc = t->cc[ch] & mask;
            if (old <= now) {
                if (cc > old && cc <= now) {
                    nrf_timer_set_event(t, ch);
                }
            } else if (cc > old || cc <= now) {
                nrf_timer_set_event(t, ch);
            }
        }
    }
    mm_nrf5340_wdt_tick(cycles);
}
