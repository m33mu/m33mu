/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>

extern void Reset_Handler(void);
extern unsigned long _estack;
extern unsigned long _image_start;
extern unsigned long _image_length;

static void default_handler(void) { while (1) { } }

void NMI_Handler(void)        __attribute__((weak, alias("default_handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("default_handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("default_handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("default_handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("default_handler")));
void SVC_Handler(void)        __attribute__((weak, alias("default_handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("default_handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("default_handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("default_handler")));

/* RW612: 16 core + up to 80 peripheral interrupts.  We size for 96 entries
 * which is plenty for the test surface and well under flash.  The boot ROM
 * reads the image length at 0x20 and the load address at 0x34; a load
 * address in the FlexSPI window makes it an execute-in-place image. */
__attribute__((section(".isr_vector")))
const uint32_t vector_table[16 + 80] = {
    [0]       = (uint32_t)&_estack,
    [1]       = (uint32_t)&Reset_Handler,
    [2]       = (uint32_t)&NMI_Handler,
    [3]       = (uint32_t)&HardFault_Handler,
    [4]       = (uint32_t)&MemManage_Handler,
    [5]       = (uint32_t)&BusFault_Handler,
    [6]       = (uint32_t)&UsageFault_Handler,
    [8]       = (uint32_t)&_image_length,
    [11]      = (uint32_t)&SVC_Handler,
    [12]      = (uint32_t)&DebugMon_Handler,
    [13]      = (uint32_t)&_image_start,
    [14]      = (uint32_t)&PendSV_Handler,
    [15]      = (uint32_t)&SysTick_Handler,
};

/* FlexSPI NOR configuration block (flexspi_nor_config_t, fsl_romapi_flexspi.h)
 * for the 64 MB W25Q512JV on FRDM-RW612: tag, flash size, page/sector/block. */
__attribute__((section(".flash_conf"), used))
const uint32_t flexspi_config[0x200 / 4] = {
    [0x000 / 4] = 0x42464346u,   /* "FCFB" */
    [0x050 / 4] = 0x04000000u,   /* sflashA1Size */
    [0x1C0 / 4] = 0x100u,        /* pageSize */
    [0x1C4 / 4] = 0x1000u,       /* sectorSize */
    [0x1D0 / 4] = 0x10000u,      /* blockSize */
};
