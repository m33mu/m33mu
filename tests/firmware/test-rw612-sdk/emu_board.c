/* m33mu -- RW612 emulator-side board bring-up.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Direct-MMIO bring-up of FlexComm0 USART for printf().  Uses the SET/CLR
 * register aliases of CLKCTL1 / RSTCTL1 — same pattern the NXP MCUXpresso
 * SDK uses on real silicon, exercising the emulator's set/clear logic.
 */
#include <stdint.h>
#include <string.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void __libc_init_array(void);
extern int main(void);
extern void _exit(int status);

/* CLKCTL1 / RSTCTL1 — FlexComm0 clock gate and reset (kCLOCK_Flexcomm0,
 * kFC0_RST_SHIFT_RSTn in the SDK) */
#define RSTCTL1_BASE     0x40020000u
#define CLKCTL1_BASE     0x40021000u
#define PSCCTL0_SET      0x40u   /* base + 0x40 */
#define PSCCTL0_CLR      0x70u   /* base + 0x70 */
#define FC0_CLK_BIT      8u

#define CLKCTL1_PSCCTL0_SET (*(volatile uint32_t *)(CLKCTL1_BASE + PSCCTL0_SET))
#define RSTCTL1_PRSTCTL0_CLR (*(volatile uint32_t *)(RSTCTL1_BASE + PSCCTL0_CLR))

/* FlexComm0 USART — 0x40106000 */
#define FC0_BASE         0x40106000u
#define FC0_PSELID       (*(volatile uint32_t *)(FC0_BASE + 0xFF8u))
#define FC0_CFG          (*(volatile uint32_t *)(FC0_BASE + 0x000u))
#define FC0_FIFOCFG      (*(volatile uint32_t *)(FC0_BASE + 0xE00u))
#define FC0_FIFOSTAT     (*(volatile uint32_t *)(FC0_BASE + 0xE04u))
#define FC0_FIFOWR       (*(volatile uint32_t *)(FC0_BASE + 0xE20u))

#define FIFOSTAT_TXNOTFULL (1u << 5)

void fc0_putc(char c)
{
    while ((FC0_FIFOSTAT & FIFOSTAT_TXNOTFULL) == 0u) { }
    FC0_FIFOWR = (uint32_t)(unsigned char)c;
}

static void fc0_init(void)
{
    /* Enable FC0 clock and release reset */
    CLKCTL1_PSCCTL0_SET   = (1u << FC0_CLK_BIT);
    RSTCTL1_PRSTCTL0_CLR  = (1u << FC0_CLK_BIT);

    /* Select USART, enable, 8N1 */
    FC0_PSELID  = (FC0_PSELID & ~0x7u) | 1u;       /* USART */
    FC0_CFG     = (1u << 0) | (1u << 2);            /* ENABLE | DATALEN=8 */
    FC0_FIFOCFG = (1u << 0) | (1u << 1);            /* TX | RX enable */
}

__attribute__((naked)) void Reset_Handler(void)
{
    __asm volatile(
        "ldr  r0, =_estack\n"
        "mov  sp, r0\n"
        "b    reset_handler_c\n"
    );
}

void reset_handler_c(void)
{
    uint32_t *src, *dst;

    src = &_sidata;
    dst = &_sdata;
    while (dst < &_edata) { *dst++ = *src++; }

    dst = &_sbss;
    while (dst < &_ebss) { *dst++ = 0u; }

    fc0_init();
    __libc_init_array();
    _exit(main());
}
