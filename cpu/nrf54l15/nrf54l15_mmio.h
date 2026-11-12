#ifndef M33MU_NRF54L15_MMIO_H
#define M33MU_NRF54L15_MMIO_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/sau.h"

struct mm_memmap;
struct mm_flash_persist;

mm_bool mm_nrf54l15_register_mmio(struct mmio_bus *bus);
void mm_nrf54l15_flash_bind(struct mm_memmap *map,
                            mm_u8 *flash,
                            mm_u32 flash_size,
                            const struct mm_flash_persist *persist,
                            mm_u32 flags);
mm_u64 mm_nrf54l15_cpu_hz(void);
void mm_nrf54l15_mmio_reset(void);
mm_bool mm_nrf54l15_tz_attr_for_addr(mm_u32 addr,
                                     enum mm_sau_attr *attr_out,
                                     mm_u32 *region_out);

#endif /* M33MU_NRF54L15_MMIO_H */
