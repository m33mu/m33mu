/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_RW612_MMIO_H
#define M33MU_RW612_MMIO_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_memmap;
struct mm_flash_persist;

mm_bool mm_rw612_register_mmio(struct mmio_bus *bus);
void mm_rw612_flash_bind(struct mm_memmap *map,
                         mm_u8 *flash,
                         mm_u32 flash_size,
                         const struct mm_flash_persist *persist,
                         mm_u32 flags);
mm_u64 mm_rw612_cpu_hz(void);
void mm_rw612_mmio_reset(void);
mm_bool mm_rw612_boot_resolve(struct mm_memmap *map,
                              const mm_u8 *flash,
                              mm_u32 flash_size,
                              mm_u32 *vtor_out);

/* Silicon revision reported in SOCCTRL CHIP_INFO[7:0]. */
mm_u32 mm_rw612_chip_rev(void);

/*
 * Returns MM_TRUE when a peripheral on a CLKCTLn / RSTCTLn bit is both
 * clocked (PSCCTL bit set) and out of reset (PRSTCTL bit clear).
 *
 * domain: 0 = CLKCTL0/RSTCTL0, 1 = CLKCTL1/RSTCTL1
 * pscctl_offset: byte offset within CLKCTLn of the PSCCTL register
 * bit:           bit number within that register
 */
mm_bool mm_rw612_clkctl_periph_active(mm_u32 domain, mm_u32 pscctl_offset, mm_u32 bit);

#endif /* M33MU_RW612_MMIO_H */
