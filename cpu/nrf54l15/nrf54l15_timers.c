/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "nrf54l15/nrf54l15_timers.h"
#include "nrf54l_mmio.h"

void mm_nrf54l15_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    nrf54l_timers_init(bus, nvic);
}

void mm_nrf54l15_timers_reset(void)
{
    nrf54l_timers_reset();
}

void mm_nrf54l15_timers_tick(mm_u64 cycles)
{
    nrf54l_timers_tick(cycles);
}
