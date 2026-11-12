/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_CPU_RW612_CONFIG_H
#define M33MU_CPU_RW612_CONFIG_H

#include "m33mu/target.h"

/*
 * NXP RW612 memory map.  The secure alias of every region is the non-secure
 * address with bit 28 set.
 *
 * The part is flashless: the boot NOR sits behind FlexSPI and is executed in
 * place through its code-bus AHB window, which is modelled as the "flash"
 * region.  The boot ROM expects the FlexSPI configuration block at offset
 * 0x400 and the image (vector table) at offset 0x1000.
 */
#define RW612_FLASH_BASE_S   0x18000000u   /* FlexSPI AHB window, secure */
#define RW612_FLASH_BASE_NS  0x08000000u
#define RW612_FLASH_SIZE     0x04000000u   /* 64 MB W25Q512JV on FRDM-RW612 */

/* 1.2 MB SRAM, system-bus alias. */
#define RW612_RAM_BASE_S     0x30000000u
#define RW612_RAM_BASE_NS    0x20000000u
#define RW612_RAM_SIZE       0x00130000u
/* The same SRAM through the code-bus alias. */
#define RW612_CODE_RAM_BASE_S  0x10000000u
#define RW612_CODE_RAM_BASE_NS 0x00000000u

#define RW612_ROM_BASE_S     0x13000000u
#define RW612_ROM_BASE_NS    0x03000000u
#define RW612_ROM_SIZE       0x00040000u

#define RW612_PERIPH_BASE_S  0x50000000u
#define RW612_PERIPH_BASE_NS 0x40000000u

/* PKA scratch RAM (data RAM exposed to the CPU) */
#define RW612_PKA_RAM_BASE   0x22040000u
#define RW612_PKA_RAM_SIZE   0x00001000u

static const struct mm_ram_region RW612_RAM_REGIONS[] = {
    { RW612_RAM_BASE_S, RW612_RAM_BASE_NS, RW612_RAM_SIZE, 0, 0u },
    { RW612_CODE_RAM_BASE_S, RW612_CODE_RAM_BASE_NS, RW612_RAM_SIZE, 0, 1u }
};

#define RW612_RAM_REGION_COUNT (sizeof(RW612_RAM_REGIONS) / sizeof(RW612_RAM_REGIONS[0]))
#define RW612_MPCBB_BLOCK_SIZE 4096u

#define RW612_SOC_RESET      mm_rw612_mmio_reset
#define RW612_SOC_REGISTER   mm_rw612_register_mmio
#define RW612_FLASH_BIND     mm_rw612_flash_bind
#define RW612_CLOCK_GET_HZ   mm_rw612_cpu_hz
#define RW612_USART_INIT     mm_rw612_flexcomm_init
#define RW612_USART_RESET    mm_rw612_flexcomm_reset
#define RW612_USART_POLL     mm_rw612_flexcomm_poll
#define RW612_SPI_INIT       mm_rw612_flexcomm_init
#define RW612_SPI_RESET      mm_rw612_flexcomm_reset
#define RW612_SPI_POLL       mm_rw612_flexcomm_poll
#define RW612_TIMER_INIT     mm_rw612_timers_init
#define RW612_TIMER_RESET    mm_rw612_timers_reset
#define RW612_TIMER_TICK     mm_rw612_timers_tick
#define RW612_BOOT_RESOLVE   mm_rw612_boot_resolve

/* The boot ROM always starts the core Secure. */
#define RW612_FLAGS (MM_TARGET_FLAG_FPU | MM_TARGET_FLAG_SECURE_BOOT)

#endif /* M33MU_CPU_RW612_CONFIG_H */
