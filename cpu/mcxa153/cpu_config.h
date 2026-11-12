#ifndef M33MU_CPU_MCXA153_CONFIG_H
#define M33MU_CPU_MCXA153_CONFIG_H

#include "m33mu/target.h"

/* NXP MCX A153: Cortex-M33 without TrustZone, DSP or FPU, so flash, RAM and
 * peripherals have one address for both security states. */
#define MCXA153_FLASH_BASE_S   0x00000000u
#define MCXA153_FLASH_BASE_NS  0x00000000u
#define MCXA153_FLASH_SIZE     0x00020000u  /* 128 KB */

#define MCXA153_RAM_BASE_S     0x20000000u
#define MCXA153_RAM_BASE_NS    0x20000000u
#define MCXA153_RAM_SIZE       0x00006000u  /* 24 KB SRAM0 + SRAM1 */

static const struct mm_ram_region MCXA153_RAM_REGIONS[] = {
    { 0x20000000u, 0x20000000u, 0x00006000u, -1, 0u }, /* 24 KB SRAM0/1 */
    { 0x04000000u, 0x04000000u, 0x00002000u, -1, 0u }  /* 8 KB SRAMX0 */
};

#define MCXA153_RAM_REGION_COUNT (sizeof(MCXA153_RAM_REGIONS) / sizeof(MCXA153_RAM_REGIONS[0]))
#define MCXA153_MPCBB_BLOCK_SIZE 0u

#define MCXA153_SOC_RESET      mm_mcxa153_mmio_reset
#define MCXA153_SOC_REGISTER   mm_mcxa153_register_mmio
#define MCXA153_FLASH_BIND     mm_mcxa153_flash_bind
#define MCXA153_CLOCK_GET_HZ   mm_mcxa153_cpu_hz
#define MCXA153_USART_INIT     mm_mcxa153_usart_init
#define MCXA153_USART_RESET    mm_mcxa153_usart_reset
#define MCXA153_USART_POLL     mm_mcxa153_usart_poll

#define MCXA153_FLAGS MM_TARGET_FLAG_NO_MPU

#endif /* M33MU_CPU_MCXA153_CONFIG_H */
