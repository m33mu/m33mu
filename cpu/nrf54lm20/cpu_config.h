#ifndef M33MU_CPU_NRF54LM20_CONFIG_H
#define M33MU_CPU_NRF54LM20_CONFIG_H

#include "m33mu/target.h"

/* Nordic nRF54LM20A application core. RRAM and RAM have one address for both
 * security states; SPU and MPC00 attribute them (cpu/nrf54l_mmio.c). */
#define NRF54LM20_FLASH_BASE_S   0x00000000u
#define NRF54LM20_FLASH_BASE_NS  0x00000000u
#define NRF54LM20_FLASH_SIZE     0x001FD000u  /* 2036 KB RRAM */

#define NRF54LM20_RAM_BASE_S     0x20000000u
#define NRF54LM20_RAM_BASE_NS    0x20000000u
#define NRF54LM20_RAM_SIZE       0x00080000u  /* 512 KB RAM (RAM + RAM2) */

#define NRF54LM20_RAM_REGIONS 0
#define NRF54LM20_RAM_REGION_COUNT 0u
#define NRF54LM20_MPCBB_BLOCK_SIZE 0u

#define NRF54LM20_SOC_RESET      mm_nrf54lm20_mmio_reset
#define NRF54LM20_SOC_REGISTER   mm_nrf54lm20_register_mmio
#define NRF54LM20_FLASH_BIND     mm_nrf54lm20_flash_bind
#define NRF54LM20_CLOCK_GET_HZ   mm_nrf54lm20_cpu_hz
#define NRF54LM20_USART_INIT     mm_nrf54lm20_usart_init
#define NRF54LM20_USART_RESET    mm_nrf54lm20_usart_reset
#define NRF54LM20_USART_POLL     mm_nrf54lm20_usart_poll
#define NRF54LM20_SPI_INIT       mm_nrf54lm20_spi_init
#define NRF54LM20_SPI_RESET      mm_nrf54lm20_spi_reset
#define NRF54LM20_SPI_POLL       mm_nrf54lm20_spi_poll
#define NRF54LM20_TIMER_INIT     mm_nrf54lm20_timers_init
#define NRF54LM20_TIMER_RESET    mm_nrf54lm20_timers_reset
#define NRF54LM20_TIMER_TICK     mm_nrf54lm20_timers_tick
#define NRF54LM20_TZ_ATTR        mm_nrf54lm20_tz_attr_for_addr

/* The core always starts Secure; TrustZone cannot be disabled. */
#define NRF54LM20_FLAGS (MM_TARGET_FLAG_FPU | MM_TARGET_FLAG_SECURE_BOOT)

#endif /* M33MU_CPU_NRF54LM20_CONFIG_H */
