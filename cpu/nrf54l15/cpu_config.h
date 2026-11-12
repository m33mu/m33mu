#ifndef M33MU_CPU_NRF54L15_CONFIG_H
#define M33MU_CPU_NRF54L15_CONFIG_H

#include "m33mu/target.h"

/* Nordic nRF54L15 application core. RRAM and RAM have one address for both
 * security states; SPU and MPC00 attribute them (cpu/nrf54l_mmio.c). */
#define NRF54L15_FLASH_BASE_S   0x00000000u
#define NRF54L15_FLASH_BASE_NS  0x00000000u
#define NRF54L15_FLASH_SIZE     0x0017D000u  /* 1524 KB RRAM */

#define NRF54L15_RAM_BASE_S     0x20000000u
#define NRF54L15_RAM_BASE_NS    0x20000000u
#define NRF54L15_RAM_SIZE       0x00040000u  /* 256 KB RAM (RAM + RAM2) */

#define NRF54L15_RAM_REGIONS 0
#define NRF54L15_RAM_REGION_COUNT 0u
#define NRF54L15_MPCBB_BLOCK_SIZE 0u

#define NRF54L15_SOC_RESET      mm_nrf54l15_mmio_reset
#define NRF54L15_SOC_REGISTER   mm_nrf54l15_register_mmio
#define NRF54L15_FLASH_BIND     mm_nrf54l15_flash_bind
#define NRF54L15_CLOCK_GET_HZ   mm_nrf54l15_cpu_hz
#define NRF54L15_USART_INIT     mm_nrf54l15_usart_init
#define NRF54L15_USART_RESET    mm_nrf54l15_usart_reset
#define NRF54L15_USART_POLL     mm_nrf54l15_usart_poll
#define NRF54L15_SPI_INIT       mm_nrf54l15_spi_init
#define NRF54L15_SPI_RESET      mm_nrf54l15_spi_reset
#define NRF54L15_SPI_POLL       mm_nrf54l15_spi_poll
#define NRF54L15_TIMER_INIT     mm_nrf54l15_timers_init
#define NRF54L15_TIMER_RESET    mm_nrf54l15_timers_reset
#define NRF54L15_TIMER_TICK     mm_nrf54l15_timers_tick
#define NRF54L15_TZ_ATTR        mm_nrf54l15_tz_attr_for_addr

/* The core always starts Secure; TrustZone cannot be disabled. */
#define NRF54L15_FLAGS (MM_TARGET_FLAG_FPU | MM_TARGET_FLAG_SECURE_BOOT)

#endif /* M33MU_CPU_NRF54L15_CONFIG_H */
