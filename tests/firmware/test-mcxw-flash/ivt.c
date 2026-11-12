/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>

extern void Reset_Handler(void);
extern unsigned long _estack;

static void default_handler(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) { }
}

__attribute__((section(".isr_vector")))
const uint32_t vector_table[16] = {
    [0] = (uint32_t)&_estack,
    [1] = (uint32_t)&Reset_Handler,
    [2 ... 15] = (uint32_t)&default_handler
};
