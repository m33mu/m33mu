#ifndef M33MU_MCXW71C_FLASH_H
#define M33MU_MCXW71C_FLASH_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_memmap;
struct mm_flash_persist;

#define MCXW71C_FMU0_BASE      0x40020000u
#define MCXW71C_FMU0_SEC_BASE  0x50020000u
#define MCXW71C_FMU0_SIZE      0x30u

#define MCXW71C_FLASH_SECTOR_SIZE 0x2000u
#define MCXW71C_FLASH_PAGE_SIZE   128u
#define MCXW71C_FLASH_PHRASE_SIZE 16u

void mm_mcxw71c_fmu_reset(void);
mm_bool mm_mcxw71c_fmu_register(struct mmio_bus *bus);
void mm_mcxw71c_fmu_bind(struct mm_memmap *map,
                         mm_u8 *flash,
                         mm_u32 flash_size,
                         const struct mm_flash_persist *persist);

#endif /* M33MU_MCXW71C_FLASH_H */
