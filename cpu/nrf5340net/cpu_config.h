#ifndef M33MU_CPU_NRF5340NET_CONFIG_H
#define M33MU_CPU_NRF5340NET_CONFIG_H

#include "m33mu/target.h"

/* Nordic nRF5340 network core: Cortex-M33 without TrustZone, DSP or FPU.
 * Flash and RAM have one address; the application core RAM is reachable as
 * the shared memory window at 0x20000000. */
#define NRF5340NET_FLASH_BASE_S   0x01000000u
#define NRF5340NET_FLASH_BASE_NS  0x01000000u
#define NRF5340NET_FLASH_SIZE     0x00040000u  /* 256 KB */

#define NRF5340NET_RAM_BASE_S     0x21000000u
#define NRF5340NET_RAM_BASE_NS    0x21000000u
#define NRF5340NET_RAM_SIZE       0x00010000u  /* 64 KB */

static const struct mm_ram_region NRF5340NET_RAM_REGIONS[] = {
    { 0x21000000u, 0x21000000u, 0x00010000u, -1, 0u }, /* network core RAM */
    { 0x20000000u, 0x20000000u, 0x00080000u, -1, 0u }  /* application core RAM */
};

#define NRF5340NET_RAM_REGION_COUNT (sizeof(NRF5340NET_RAM_REGIONS) / sizeof(NRF5340NET_RAM_REGIONS[0]))
#define NRF5340NET_MPCBB_BLOCK_SIZE 0u

#define NRF5340NET_SOC_RESET      mm_nrf5340net_mmio_reset
#define NRF5340NET_SOC_REGISTER   mm_nrf5340net_register_mmio
#define NRF5340NET_FLASH_BIND     mm_nrf5340_flash_bind
#define NRF5340NET_CLOCK_GET_HZ   mm_nrf5340_cpu_hz
#define NRF5340NET_USART_INIT     mm_nrf5340net_usart_init
#define NRF5340NET_USART_RESET    mm_nrf5340net_usart_reset
#define NRF5340NET_USART_POLL     mm_nrf5340net_usart_poll
#define NRF5340NET_SPI_INIT       mm_nrf5340net_spi_init
#define NRF5340NET_SPI_RESET      mm_nrf5340net_spi_reset
#define NRF5340NET_SPI_POLL       mm_nrf5340net_spi_poll
#define NRF5340NET_TIMER_INIT     mm_nrf5340net_timers_init
#define NRF5340NET_TIMER_RESET    mm_nrf5340net_timers_reset
#define NRF5340NET_TIMER_TICK     mm_nrf5340net_timers_tick

#define NRF5340NET_FLAGS 0u

#endif /* M33MU_CPU_NRF5340NET_CONFIG_H */
