#ifndef M33MU_MCXA153_ROMAPI_H
#define M33MU_MCXA153_ROMAPI_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"
#include "m33mu/flash_persist.h"

/* Boot ROM window that holds the bootloader tree and the stub slots */
#define MCXA153_ROMAPI_BASE 0x03003000u
#define MCXA153_ROMAPI_SIZE 0x00001000u

mm_bool mm_mcxa153_romapi_register_mmio(struct mmio_bus *bus);
void mm_mcxa153_romapi_reset(void);
mm_bool mm_mcxa153_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map);
void mm_mcxa153_romapi_set_persist(const struct mm_flash_persist *persist);

#endif /* M33MU_MCXA153_ROMAPI_H */
