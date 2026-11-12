/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_PSC3M6_ROMAPI_H
#define M33MU_PSC3M6_ROMAPI_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"
#include "m33mu/flash_persist.h"

/* Boot ROM flash API (cyboot_flash_list.h): the rom_funct_list_t table at
 * 0x1080FF6C and the blocking row operations it points at. */
#define PSC3M6_ROM_FUNC_TABLE   0x1080FF6Cu
#define PSC3M6_ROM_FUNC_COUNT   35u
#define PSC3M6_ROM_FUNC_ERASE_ROW   29u
#define PSC3M6_ROM_FUNC_PROGRAM_ROW 30u
#define PSC3M6_ROM_FUNC_WRITE_ROW   31u

#define PSC3M6_FLASH_ROW_SIZE   512u
#define PSC3M6_FLASH_COL33_SIZE 16u
#define PSC3M6_FLASH_SECTOR_SIZE (128u * 1024u)

#define CYBOOT_FLASH_SUCCESS        0x0D50B002u
#define CYBOOT_FLASH_FAILED         0xBAF00008u
#define CYBOOT_FLASH_ADDR_INVALID   0xBAF0004Cu
#define CYBOOT_FLASH_PARAM_INVALID  0xBAF0004Du

mm_bool mm_psc3m6_romapi_register_mmio(struct mmio_bus *bus);
void    mm_psc3m6_romapi_reset(void);
void    mm_psc3m6_romapi_bind(struct mm_memmap *map, mm_u8 *flash, mm_u32 flash_size,
                              const struct mm_flash_persist *persist);
mm_bool mm_psc3m6_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map);

#endif /* M33MU_PSC3M6_ROMAPI_H */
