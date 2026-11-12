/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;
extern void __libc_init_array(void);

/* SYSCON */
#define SYSCON_BASE        0x40000000u
#define SYSCON_PRESETCTRL0 0x100u
#define SYSCON_AHBCLKCTRL0 0x200u

static inline void syscon_enable(uint32_t bit)
{
    volatile uint32_t *clk_reg = (volatile uint32_t *)(SYSCON_BASE + SYSCON_AHBCLKCTRL0);
    volatile uint32_t *rst_reg = (volatile uint32_t *)(SYSCON_BASE + SYSCON_PRESETCTRL0);
    *clk_reg |= (1u << bit);
    *rst_reg |= (1u << bit);
}

/* PORT0 */
#define PORT0_BASE       0x40116000u
#define PORT0_PCR0       (*(volatile uint32_t *)(PORT0_BASE + 0x20u))

/* GPIO0 */
#define GPIO0_BASE       0x40096000u
#define GPIO_PDOR(b)     (*(volatile uint32_t *)((b) + 0x20u))
#define GPIO_PSOR(b)     (*(volatile uint32_t *)((b) + 0x24u))
#define GPIO_PCOR(b)     (*(volatile uint32_t *)((b) + 0x28u))
#define GPIO_PDDR(b)     (*(volatile uint32_t *)((b) + 0x34u))

/* S50 ELS and AHB secure controller */
#define ELS_BASE         0x40054000u
#define ELS_STATUS       (*(volatile uint32_t *)(ELS_BASE + 0x00u))
#define ELS_CTRL         (*(volatile uint32_t *)(ELS_BASE + 0x04u))
#define ELS_CMDCFG0      (*(volatile uint32_t *)(ELS_BASE + 0x08u))
#define ELS_DMA_RES0     (*(volatile uint32_t *)(ELS_BASE + 0x38u))
#define ELS_DMA_RES0_LEN (*(volatile uint32_t *)(ELS_BASE + 0x3Cu))
#define ELS_STATUS_BUSY  (1u << 0)
#define ELS_STATUS_ERR   (1u << 2)
#define ELS_CTRL_EN      (1u << 0)
#define ELS_CTRL_START   (1u << 1)
#define ELS_CMD_RND_REQ  (24u << 3)
#define AHBSC_MISC_CTRL_REG (*(volatile uint32_t *)(0x40120000u + 0xFFCu))

/* LP_FLEXCOMM0 (UART mode) */
#define FLEXCOMM0_BASE   0x40092000u
#define FLEXCOMM_PSELID  0xFF8u
#define PSELID_PERSEL_UART 1u

#define LPUART_STAT(b)   (*(volatile uint32_t *)((b) + 0x14u))
#define LPUART_CTRL(b)   (*(volatile uint32_t *)((b) + 0x18u))
#define LPUART_DATA(b)   (*(volatile uint32_t *)((b) + 0x1Cu))
#define LPUART_STAT_TDRE (1u << 23)
#define LPUART_CTRL_TE   (1u << 19)

/* LP_FLEXCOMM1 (SPI mode) */
#define FLEXCOMM1_BASE   0x40093000u
#define PSELID_PERSEL_SPI 2u

#define LPSPI_CR(b)      (*(volatile uint32_t *)((b) + 0x10u))
#define LPSPI_SR(b)      (*(volatile uint32_t *)((b) + 0x14u))
#define LPSPI_TCR(b)     (*(volatile uint32_t *)((b) + 0x60u))
#define LPSPI_TDR(b)     (*(volatile uint32_t *)((b) + 0x64u))
#define LPSPI_RDR(b)     (*(volatile uint32_t *)((b) + 0x74u))
#define LPSPI_CR_MEN     (1u << 0)
#define LPSPI_SR_TDF     (1u << 0)
#define LPSPI_SR_RDF     (1u << 1)
#define LPSPI_TCR_FRAMESZ(x) ((x) & 0x1Fu)
#define LPSPI_TCR_CONT   (1u << 21)

/* SysTick */
#define SYST_CSR          (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR          (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR          (*(volatile uint32_t *)0xE000E018u)
#define SYST_CSR_ENABLE   (1u << 0)
#define SYST_CSR_TICKINT  (1u << 1)
#define SYST_CSR_CLKSRC   (1u << 2)

volatile uint32_t systick_ms = 0;

static void lpuart0_init(void)
{
    volatile uint32_t *pselid = (volatile uint32_t *)(FLEXCOMM0_BASE + FLEXCOMM_PSELID);

    /* Enable FLEXCOMM0 clock and reset */
    syscon_enable(11); /* FC0 at bit 11 */

    /* Configure FLEXCOMM0 as UART */
    *pselid = (*pselid & ~0x7u) | PSELID_PERSEL_UART;

    /* Enable transmitter */
    LPUART_CTRL(FLEXCOMM0_BASE) = LPUART_CTRL_TE;
}

void lpuart0_putc(char c)
{
    while ((LPUART_STAT(FLEXCOMM0_BASE) & LPUART_STAT_TDRE) == 0u) {
    }
    LPUART_DATA(FLEXCOMM0_BASE) = (uint32_t)c;
}

static void gpio_cs_init(void)
{
    /* Enable PORT0 and GPIO0 clocks and resets */
    syscon_enable(13); /* PORT0 at bit 13 */
    syscon_enable(19); /* GPIO0 at bit 19 */

    /* Configure P0.0 as GPIO */
    PORT0_PCR0 = (1u << 8); /* GPIO mux (ALT1) */

    /* Set P0.0 as output, initial high (CS inactive) */
    GPIO_PDDR(GPIO0_BASE) |= 1u << 0;
    GPIO_PSOR(GPIO0_BASE) = 1u << 0;
}

static void spi1_init(void)
{
    volatile uint32_t *pselid = (volatile uint32_t *)(FLEXCOMM1_BASE + FLEXCOMM_PSELID);

    /* Enable FLEXCOMM1 clock and reset */
    syscon_enable(12); /* FC1 at bit 12 */

    /* Configure FLEXCOMM1 as SPI */
    *pselid = (*pselid & ~0x7u) | PSELID_PERSEL_SPI;

    /* Configure SPI: 8-bit frames, continuous */
    LPSPI_CR(FLEXCOMM1_BASE) = 0;
    LPSPI_TCR(FLEXCOMM1_BASE) = LPSPI_TCR_FRAMESZ(7u) | LPSPI_TCR_CONT;
    LPSPI_CR(FLEXCOMM1_BASE) = LPSPI_CR_MEN;
}

static uint8_t spi1_xfer(uint8_t v)
{
    uint8_t rx;

    /* Wait for TX FIFO ready */
    while ((LPSPI_SR(FLEXCOMM1_BASE) & LPSPI_SR_TDF) == 0u) {
    }

    /* Transmit byte */
    LPSPI_TDR(FLEXCOMM1_BASE) = v;

    /* Wait for RX data */
    while ((LPSPI_SR(FLEXCOMM1_BASE) & LPSPI_SR_RDF) == 0u) {
    }

    /* Read received byte */
    rx = (uint8_t)(LPSPI_RDR(FLEXCOMM1_BASE) & 0xFFu);

    return rx;
}

static void systick_init(uint32_t cpu_hz)
{
    uint32_t reload = (cpu_hz / 1000u) - 1u;
    SYST_CVR = 0u;
    SYST_RVR = reload;
    SYST_CSR = SYST_CSR_CLKSRC | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

void SysTick_Handler(void)
{
    ++systick_ms;
}

static uint32_t wait_systick(uint32_t target_ms)
{
    uint32_t start = systick_ms;
    volatile uint32_t spin = 0;
    while ((uint32_t)(systick_ms - start) < target_ms) {
        if (++spin > 5000000u) {
            break;
        }
    }
    return (uint32_t)(systick_ms - start);
}

/* the wolfBoot hal_trng sequence: enable, then RND_REQ DMA into SRAM */
static uint32_t els_rnd_buf[8] __attribute__((aligned(4)));

static int els_rnd_req_ok(void)
{
    uint32_t first;
    ELS_CTRL = ELS_CTRL_EN;
    while ((ELS_STATUS & ELS_STATUS_BUSY) != 0u) {
    }
    ELS_DMA_RES0 = (uint32_t)els_rnd_buf;
    ELS_DMA_RES0_LEN = sizeof(els_rnd_buf);
    ELS_CMDCFG0 = 0u;
    ELS_CTRL = ELS_CTRL_EN | ELS_CTRL_START | ELS_CMD_RND_REQ;
    while ((ELS_STATUS & ELS_STATUS_BUSY) != 0u) {
    }
    if ((ELS_STATUS & ELS_STATUS_ERR) != 0u) {
        return 0;
    }
    first = els_rnd_buf[0];
    ELS_CTRL = ELS_CTRL_EN | ELS_CTRL_START | ELS_CMD_RND_REQ;
    while ((ELS_STATUS & ELS_STATUS_BUSY) != 0u) {
    }
    return ((ELS_STATUS & ELS_STATUS_ERR) == 0u && els_rnd_buf[0] != first) ? 1 : 0;
}

int main(void)
{
    uint8_t id0, id1, id2;

    lpuart0_init();
    gpio_cs_init();
    spi1_init();
    __asm volatile("cpsie i");
    systick_init(150000000u); /* 150 MHz */

    printf("MCXN947 test start\n");
    printf("FLEXCOMM0: UART mode, FLEXCOMM1: SPI mode\n");

    /* Read SPI flash JEDEC ID */
    GPIO_PCOR(GPIO0_BASE) = 1u << 0; /* CS low */
    spi1_xfer(0x9Fu); /* JEDEC ID command */
    id0 = spi1_xfer(0xFFu);
    id1 = spi1_xfer(0xFFu);
    id2 = spi1_xfer(0xFFu);
    GPIO_PSOR(GPIO0_BASE) = 1u << 0; /* CS high */

    printf("JEDEC ID: %02x %02x %02x\n", id0, id1, id2);
    printf("SysTick ms: %lu\n", (unsigned long)systick_ms);
    printf("SysTick +%lu ms\n", (unsigned long)wait_systick(5u));
    printf("ELS RND_REQ: %s\n", els_rnd_req_ok() ? "PASS" : "FAIL");
    printf("AHBSC MISC_CTRL: %s\n", (AHBSC_MISC_CTRL_REG == 0x0000AAAAu) ? "PASS" : "FAIL");

    printf("MCXN947 test done\n");

    __asm volatile("bkpt #0x7f");
    while (1) {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }

    __libc_init_array();
    main();
}

void HardFault_Handler(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
    }
}
