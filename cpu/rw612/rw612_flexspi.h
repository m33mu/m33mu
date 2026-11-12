/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_RW612_FLEXSPI_H
#define M33MU_RW612_FLEXSPI_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_memmap;
struct mm_flash_persist;

mm_bool mm_rw612_flexspi_register_mmio(struct mmio_bus *bus);
void mm_rw612_flexspi_reset(void);
void mm_rw612_flexspi_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist);

/*
 * Serial NOR behind FlexSPI port A1, addressed by NOR offset (the numbering
 * the boot ROM FlexSPI NOR driver uses).  Program follows NOR semantics: it
 * can only clear bits, so 0xFF padding leaves existing data untouched.
 */
mm_u32 mm_rw612_nor_size(void);
mm_bool mm_rw612_nor_program(mm_u32 off, const mm_u8 *data, mm_u32 len);
mm_bool mm_rw612_nor_erase(mm_u32 off, mm_u32 len);
mm_bool mm_rw612_nor_read(mm_u32 off, mm_u8 *out, mm_u32 len);

#endif /* M33MU_RW612_FLEXSPI_H */
