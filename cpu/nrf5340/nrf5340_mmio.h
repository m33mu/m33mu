#ifndef M33MU_NRF5340_MMIO_H
#define M33MU_NRF5340_MMIO_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

struct mm_memmap;
struct mm_flash_persist;

mm_bool mm_nrf5340_register_mmio(struct mmio_bus *bus);
/* Network core: the same blocks at the 0x41xxxxxx bases, 2 KB NVMC pages */
mm_bool mm_nrf5340net_register_mmio(struct mmio_bus *bus);
void mm_nrf5340_flash_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist,
                           mm_u32 flags);
mm_u64 mm_nrf5340_cpu_hz(void);
mm_bool mm_nrf5340_clock_hf_running(void);
mm_bool mm_nrf5340_clock_lf_running(void);
void mm_nrf5340_rtc_tick(mm_u64 cycles);
void mm_nrf5340_mmio_reset(void);
void mm_nrf5340net_mmio_reset(void);
/* Wire NVIC into peripherals that raise IRQs (e.g. CryptoCell-312).
 * Called from mm_nrf5340_timers_init after NVIC is available. */
void mm_nrf5340_set_nvic(struct mm_nvic *nvic);

#endif /* M33MU_NRF5340_MMIO_H */
