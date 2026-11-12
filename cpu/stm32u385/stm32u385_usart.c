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

#include "stm32u385/stm32u385_usart.h"
#include "stm32u385/stm32u385_mmio.h"
#include "stm32_usart.h"

static struct stm32_usart_state g_usart;

/* RCC_APB2ENR.USART1EN (RM0487) */
static mm_bool clock_apb2_usart1(struct stm32_usart_inst *u)
{
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0xa4 / 4] >> 14) & 1u) != 0u;
}

/* RCC_APB1ENR1: USART3EN bit 18, UART4EN bit 19, UART5EN bit 20 */
static mm_bool clock_apb1enr1_generic(struct stm32_usart_inst *u)
{
    mm_u32 bit;
    switch (u->index) {
    case 1: bit = 18; break;
    case 2: bit = 19; break;
    case 3: bit = 20; break;
    default: return MM_TRUE;
    }
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0x9c / 4] >> bit) & 1u) != 0u;
}

/* RCC_APB3ENR.LPUART1EN */
static mm_bool clock_apb3_lpuart1(struct stm32_usart_inst *u)
{
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0xa8 / 4] >> 6) & 1u) != 0u;
}

void mm_stm32u385_usart_poll(void)
{
    stm32_usart_poll(&g_usart);
}

void mm_stm32u385_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40013800u, 0x40004800u, 0x40004C00u, 0x40005000u, 0x46002400u
    };
    static const int irq_map[] = { 61, 63, 64, 65, 66 };
    static const char *labels[] = { "USART1", "USART3", "UART4", "UART5", "LPUART1" };
    size_t i;
    stm32_usart_state_init(&g_usart, sizeof(bases) / sizeof(bases[0]), nvic);
    for (i = 0; i < g_usart.usart_count; ++i) {
        struct stm32_usart_inst *u;
        stm32_usart_register_instance(&g_usart, bus, i, bases[i], irq_map[i], labels[i],
                                      MM_FALSE, MM_TRUE, MM_FALSE);
        u = &g_usart.usarts[i];
        u->rcc_regs = mm_stm32u385_rcc_regs();
        if (i == 0) {
            u->clock_on = clock_apb2_usart1;
        } else if (i <= 3) {
            u->clock_on = clock_apb1enr1_generic;
        } else {
            u->clock_on = clock_apb3_lpuart1;
        }
    }
}

void mm_stm32u385_usart_reset(void)
{
    stm32_usart_reset(&g_usart);
}
