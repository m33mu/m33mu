/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_PSC3M6_MMIO_H
#define M33MU_PSC3M6_MMIO_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_memmap;
struct mm_flash_persist;
struct mm_nvic;

mm_bool mm_psc3m6_register_mmio(struct mmio_bus *bus);
void mm_psc3m6_mmio_reset(void);
void mm_psc3m6_flash_bind(struct mm_memmap *map,
                          mm_u8 *flash,
                          mm_u32 flash_size,
                          const struct mm_flash_persist *persist,
                          mm_u32 flags);
mm_u64 mm_psc3m6_cpu_hz(void);

/* PERI_PCLK: MM_TRUE when CLOCK_CTL[idx] of group gr selects a divider that
 * DIV_CMD has started. */
mm_bool mm_psc3m6_pclk_active(mm_u32 gr, mm_u32 idx);
/* PERI: MM_TRUE when GR[gr].SL_CTL enables the slave. */
mm_bool mm_psc3m6_peri_slave_enabled(mm_u32 gr, mm_u32 slave);

/* SCB0..SCB5 as UART or SPI master. */
void mm_psc3m6_scb_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_psc3m6_scb_reset(void);
void mm_psc3m6_scb_poll(void);

#endif /* M33MU_PSC3M6_MMIO_H */
