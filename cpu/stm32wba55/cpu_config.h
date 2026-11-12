/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#ifndef M33MU_CPU_STM32WBA55_CONFIG_H
#define M33MU_CPU_STM32WBA55_CONFIG_H

/* STM32WBA55CG memory map (RM0493): 1 MB single-bank flash, 128 KB SRAM,
 * Secure / Non-secure aliases. */
#include "m33mu/target.h"

#define STM32WBA55_FLASH_BASE_S   0x0C000000u
#define STM32WBA55_FLASH_BASE_NS  0x08000000u
#define STM32WBA55_FLASH_SIZE     0x00100000u  /* 1 MB */

#define STM32WBA55_RAM_BASE_S     0x30000000u
#define STM32WBA55_RAM_BASE_NS    0x20000000u
#define STM32WBA55_RAM_SIZE       0x00020000u  /* 128 KB */

static const struct mm_ram_region STM32WBA55_RAM_REGIONS[] = {
    { 0x30000000u, 0x20000000u, 0x00010000u, -1, 0u }, /* SRAM1 64 KB */
    { 0x30010000u, 0x20010000u, 0x00010000u, -1, 0u }  /* SRAM2 64 KB */
};

#define STM32WBA55_RAM_REGION_COUNT (sizeof(STM32WBA55_RAM_REGIONS) / sizeof(STM32WBA55_RAM_REGIONS[0]))

#define STM32WBA55_SOC_RESET      mm_stm32wba55_mmio_reset
#define STM32WBA55_SOC_REGISTER   mm_stm32wba55_register_mmio
#define STM32WBA55_FLASH_BIND     mm_stm32wba55_flash_bind
#define STM32WBA55_CLOCK_GET_HZ   mm_stm32wba55_cpu_hz
#define STM32WBA55_USART_INIT     mm_stm32wba55_usart_init
#define STM32WBA55_USART_RESET    mm_stm32wba55_usart_reset
#define STM32WBA55_USART_POLL     mm_stm32wba55_usart_poll

#define STM32WBA55_FLAGS (MM_TARGET_FLAG_NVM_WRITEONCE | MM_TARGET_FLAG_FPU)

#endif /* M33MU_CPU_STM32WBA55_CONFIG_H */
