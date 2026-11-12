#ifndef M33MU_MCXA153_MMIO_H
#define M33MU_MCXA153_MMIO_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_memmap;
struct mm_flash_persist;

mm_bool mm_mcxa153_register_mmio(struct mmio_bus *bus);
void mm_mcxa153_flash_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist,
                           mm_u32 flags);
mm_u64 mm_mcxa153_cpu_hz(void);
void mm_mcxa153_mmio_reset(void);

/* MRCC_GLB_CC0 / MRCC_GLB_RST0 bit numbers (fsl_clock.h, fsl_reset.h) */
#define MCXA153_MRCC_LPUART0 19u
#define MCXA153_MRCC_LPUART1 20u
#define MCXA153_MRCC_LPUART2 21u

/* Clock gated on and reset released for a MRCC_GLB_*0 bit */
mm_bool mm_mcxa153_mrcc_enabled(mm_u32 bit);

#endif /* M33MU_MCXA153_MMIO_H */
