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

/* GPIO P0 */
#define GPIO_P0_BASE    0x5010A000u
#define GPIO_OUT(b)     (*(volatile uint32_t *)((b) + 0x000u))
#define GPIO_OUTSET(b)  (*(volatile uint32_t *)((b) + 0x004u))
#define GPIO_OUTCLR(b)  (*(volatile uint32_t *)((b) + 0x008u))
#define GPIO_DIRSET(b)  (*(volatile uint32_t *)((b) + 0x014u))

/* UARTE00 */
#define UARTE00_BASE      0x5004D000u
#define UARTE_TASKS_DMA_TX_START(b) (*(volatile uint32_t *)((b) + 0x050u))
#define UARTE_EVENTS_DMA_TX_END(b)  (*(volatile uint32_t *)((b) + 0x168u))
#define UARTE_ENABLE(b)        (*(volatile uint32_t *)((b) + 0x500u))
#define UARTE_DMA_TX_PTR(b)    (*(volatile uint32_t *)((b) + 0x73Cu))
#define UARTE_DMA_TX_MAXCNT(b) (*(volatile uint32_t *)((b) + 0x740u))
#define UARTE_DMA_TX_AMOUNT(b) (*(volatile uint32_t *)((b) + 0x744u))

/* SPIM20 */
#define SPIM20_BASE       0x500C6000u
#define SPIM_TASKS_START(b) (*(volatile uint32_t *)((b) + 0x000u))
#define SPIM_EVENTS_END(b)  (*(volatile uint32_t *)((b) + 0x108u))
#define SPIM_ENABLE(b)      (*(volatile uint32_t *)((b) + 0x500u))
#define SPIM_RXD_PTR(b)     (*(volatile uint32_t *)((b) + 0x704u))
#define SPIM_RXD_MAXCNT(b)  (*(volatile uint32_t *)((b) + 0x708u))
#define SPIM_RXD_AMOUNT(b)  (*(volatile uint32_t *)((b) + 0x70Cu))
#define SPIM_TXD_PTR(b)     (*(volatile uint32_t *)((b) + 0x73Cu))
#define SPIM_TXD_MAXCNT(b)  (*(volatile uint32_t *)((b) + 0x740u))
#define SPIM_TXD_AMOUNT(b)  (*(volatile uint32_t *)((b) + 0x744u))
#define SPIM_ORC(b)         (*(volatile uint32_t *)((b) + 0x5C0u))

/* RRAMC */
#define RRAMC_BASE         0x5004E000u
#define RRAMC_READY(b)     (*(volatile uint32_t *)((b) + 0x400u))
#define RRAMC_READYNEXT(b) (*(volatile uint32_t *)((b) + 0x404u))
#define RRAMC_CONFIG(b)    (*(volatile uint32_t *)((b) + 0x500u))
#define RRAMC_ERASEALL(b)  (*(volatile uint32_t *)((b) + 0x540u))

/* CLOCK */
#define CLOCK_BASE         0x5010E000u
#define CLOCK_TASKS_XOSTART(b)   (*(volatile uint32_t *)((b) + 0x000u))
#define CLOCK_TASKS_XOSTOP(b)    (*(volatile uint32_t *)((b) + 0x004u))
#define CLOCK_TASKS_PLLSTART(b)  (*(volatile uint32_t *)((b) + 0x008u))
#define CLOCK_TASKS_PLLSTOP(b)   (*(volatile uint32_t *)((b) + 0x00Cu))
#define CLOCK_TASKS_LFCLKSTART(b) (*(volatile uint32_t *)((b) + 0x010u))
#define CLOCK_TASKS_LFCLKSTOP(b)  (*(volatile uint32_t *)((b) + 0x014u))
#define CLOCK_EVENTS_XOSTARTED(b)  (*(volatile uint32_t *)((b) + 0x100u))
#define CLOCK_EVENTS_PLLSTARTED(b) (*(volatile uint32_t *)((b) + 0x104u))
#define CLOCK_EVENTS_LFCLKSTARTED(b) (*(volatile uint32_t *)((b) + 0x108u))
#define CLOCK_XO_RUN(b)    (*(volatile uint32_t *)((b) + 0x408u))
#define CLOCK_XO_STAT(b)   (*(volatile uint32_t *)((b) + 0x40Cu))
#define CLOCK_PLL_RUN(b)   (*(volatile uint32_t *)((b) + 0x428u))
#define CLOCK_PLL_STAT(b)  (*(volatile uint32_t *)((b) + 0x42Cu))
#define CLOCK_LFCLK_SRC(b) (*(volatile uint32_t *)((b) + 0x440u))
#define CLOCK_LFCLK_RUN(b) (*(volatile uint32_t *)((b) + 0x448u))
#define CLOCK_LFCLK_STAT(b) (*(volatile uint32_t *)((b) + 0x44Cu))
#define CLOCK_LFCLK_SRCCOPY(b) (*(volatile uint32_t *)((b) + 0x450u))

/* OSCILLATORS */
#define OSC_BASE          0x50120000u
#define OSC_PLL_FREQ(b)   (*(volatile uint32_t *)((b) + 0x800u))
#define OSC_PLL_CURRENTFREQ(b) (*(volatile uint32_t *)((b) + 0x804u))

/* GRTC */
#define GRTC_BASE         0x500E2000u
#define GRTC_TASKS_START(b) (*(volatile uint32_t *)((b) + 0x060u))
#define GRTC_TASKS_CLEAR(b) (*(volatile uint32_t *)((b) + 0x068u))
#define GRTC_EVENTS_COMPARE0(b) (*(volatile uint32_t *)((b) + 0x100u))
#define GRTC_MODE(b)      (*(volatile uint32_t *)((b) + 0x510u))
#define GRTC_CC0_CCL(b)   (*(volatile uint32_t *)((b) + 0x520u))
#define GRTC_CC0_CCH(b)   (*(volatile uint32_t *)((b) + 0x524u))
#define GRTC_CC0_CCEN(b)  (*(volatile uint32_t *)((b) + 0x52Cu))
#define GRTC_INTERVAL(b)  (*(volatile uint32_t *)((b) + 0x6A8u))
#define GRTC_SYSCOUNTERL(b) (*(volatile uint32_t *)((b) + 0x720u))
#define GRTC_SYSCOUNTERH(b) (*(volatile uint32_t *)((b) + 0x724u))

static volatile uint8_t uarte00_txbuf;

__attribute__((section(".testpage")))
volatile uint32_t test_page[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };

static void uarte00_init(void)
{
    UARTE_ENABLE(UARTE00_BASE) = 8u;
}

void uarte00_putc(char c)
{
    uarte00_txbuf = (uint8_t)c;
    UARTE_DMA_TX_PTR(UARTE00_BASE) = (uint32_t)&uarte00_txbuf;
    UARTE_DMA_TX_MAXCNT(UARTE00_BASE) = 1u;
    UARTE_EVENTS_DMA_TX_END(UARTE00_BASE) = 0u;
    UARTE_TASKS_DMA_TX_START(UARTE00_BASE) = 1u;
    while (UARTE_EVENTS_DMA_TX_END(UARTE00_BASE) == 0u) {
    }
    (void)UARTE_DMA_TX_AMOUNT(UARTE00_BASE);
}

static void gpio_cs_init(void)
{
    GPIO_DIRSET(GPIO_P0_BASE) = 1u << 0;
    GPIO_OUTSET(GPIO_P0_BASE) = 1u << 0;
}

static void spim20_init(void)
{
    SPIM_ORC(SPIM20_BASE) = 0xFFu;
    SPIM_ENABLE(SPIM20_BASE) = 7u;
}

static void spim20_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPIM_TXD_PTR(SPIM20_BASE) = (uint32_t)tx;
    SPIM_TXD_MAXCNT(SPIM20_BASE) = len;
    SPIM_RXD_PTR(SPIM20_BASE) = (uint32_t)rx;
    SPIM_RXD_MAXCNT(SPIM20_BASE) = len;
    SPIM_EVENTS_END(SPIM20_BASE) = 0u;
    SPIM_TASKS_START(SPIM20_BASE) = 1u;
    while (SPIM_EVENTS_END(SPIM20_BASE) == 0u) {
        __asm volatile("wfi");
    }
    (void)SPIM_RXD_AMOUNT(SPIM20_BASE);
    (void)SPIM_TXD_AMOUNT(SPIM20_BASE);
}

static uint64_t grtc_syscounter_get(void)
{
    uint32_t lo = GRTC_SYSCOUNTERL(GRTC_BASE);
    uint32_t hi = GRTC_SYSCOUNTERH(GRTC_BASE) & 0x000FFFFFu;
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void grtc_wait_us(uint32_t us)
{
    uint64_t start = grtc_syscounter_get();
    while ((uint64_t)(grtc_syscounter_get() - start) < (uint64_t)us) {
        __asm volatile("wfi");
    }
}

static void clock_test(void)
{
    CLOCK_EVENTS_XOSTARTED(CLOCK_BASE) = 0u;
    CLOCK_TASKS_XOSTART(CLOCK_BASE) = 1u;
    while (CLOCK_EVENTS_XOSTARTED(CLOCK_BASE) == 0u) {
        __asm volatile("wfi");
    }

    CLOCK_EVENTS_PLLSTARTED(CLOCK_BASE) = 0u;
    CLOCK_TASKS_PLLSTART(CLOCK_BASE) = 1u;
    while (CLOCK_EVENTS_PLLSTARTED(CLOCK_BASE) == 0u) {
        __asm volatile("wfi");
    }

    CLOCK_LFCLK_SRC(CLOCK_BASE) = 0u;
    CLOCK_EVENTS_LFCLKSTARTED(CLOCK_BASE) = 0u;
    CLOCK_TASKS_LFCLKSTART(CLOCK_BASE) = 1u;
    while (CLOCK_EVENTS_LFCLKSTARTED(CLOCK_BASE) == 0u) {
        __asm volatile("wfi");
    }

    printf("CLOCK XO RUN=%lu STAT=%lu\n",
           (unsigned long)CLOCK_XO_RUN(CLOCK_BASE),
           (unsigned long)CLOCK_XO_STAT(CLOCK_BASE));
    printf("CLOCK PLL RUN=%lu STAT=%lu\n",
           (unsigned long)CLOCK_PLL_RUN(CLOCK_BASE),
           (unsigned long)CLOCK_PLL_STAT(CLOCK_BASE));
    printf("CLOCK LFCLK RUN=%lu STAT=%lu SRC=%lu SRCCOPY=%lu\n",
           (unsigned long)CLOCK_LFCLK_RUN(CLOCK_BASE),
           (unsigned long)CLOCK_LFCLK_STAT(CLOCK_BASE),
           (unsigned long)CLOCK_LFCLK_SRC(CLOCK_BASE),
           (unsigned long)CLOCK_LFCLK_SRCCOPY(CLOCK_BASE));

    CLOCK_TASKS_LFCLKSTOP(CLOCK_BASE) = 1u;
    CLOCK_TASKS_PLLSTOP(CLOCK_BASE) = 1u;
    CLOCK_TASKS_XOSTOP(CLOCK_BASE) = 1u;
}

static void osc_test(void)
{
    uint32_t cur;
    cur = OSC_PLL_CURRENTFREQ(OSC_BASE);
    printf("OSC PLL CURRENTFREQ=%lu\n", (unsigned long)cur);
    OSC_PLL_FREQ(OSC_BASE) = 0x1u;
    printf("OSC PLL FREQ=1 CURRENTFREQ=%lu\n", (unsigned long)OSC_PLL_CURRENTFREQ(OSC_BASE));
    OSC_PLL_FREQ(OSC_BASE) = 0x3u;
    printf("OSC PLL FREQ=3 CURRENTFREQ=%lu\n", (unsigned long)OSC_PLL_CURRENTFREQ(OSC_BASE));
}

static void grtc_test(void)
{
    uint64_t now;
    uint64_t cc0;

    GRTC_TASKS_CLEAR(GRTC_BASE) = 1u;
    GRTC_MODE(GRTC_BASE) = (1u << 1);
    GRTC_TASKS_START(GRTC_BASE) = 1u;

    now = grtc_syscounter_get();
    grtc_wait_us(1000u);
    printf("GRTC SYSCOUNTER delta=%lu us\n",
           (unsigned long)(grtc_syscounter_get() - now));

    now = grtc_syscounter_get();
    cc0 = now + 2000u;
    GRTC_CC0_CCL(GRTC_BASE) = (uint32_t)(cc0 & 0xFFFFFFFFu);
    GRTC_CC0_CCH(GRTC_BASE) = (uint32_t)((cc0 >> 32) & 0xFFFFFFFFu);
    GRTC_CC0_CCEN(GRTC_BASE) = 1u;
    GRTC_EVENTS_COMPARE0(GRTC_BASE) = 0u;
    while (GRTC_EVENTS_COMPARE0(GRTC_BASE) == 0u) {
        __asm volatile("wfi");
    }
    printf("GRTC COMPARE0 fired\n");

    GRTC_INTERVAL(GRTC_BASE) = 500u;
    GRTC_EVENTS_COMPARE0(GRTC_BASE) = 0u;
    while (GRTC_EVENTS_COMPARE0(GRTC_BASE) == 0u) {
        __asm volatile("wfi");
    }
    printf("GRTC COMPARE0 interval fired\n");
}

static void rramc_test(void)
{
    volatile uint32_t *page = &test_page[0];

    printf("RRAMC READY=%lu NEXT=%lu\n",
           (unsigned long)RRAMC_READY(RRAMC_BASE),
           (unsigned long)RRAMC_READYNEXT(RRAMC_BASE));

    printf("RRAMC test page before: 0x%08lx\n", (unsigned long)page[0]);
    RRAMC_CONFIG(RRAMC_BASE) = 1u;
    page[0] = 0xA5A5A5A5u;
    printf("RRAMC test page after write: 0x%08lx\n", (unsigned long)page[0]);

    (void)RRAMC_ERASEALL(RRAMC_BASE);
    printf("RRAMC test page after eraseall (emulated): 0x%08lx\n", (unsigned long)page[0]);
}

int main(void)
{
    uint8_t tx[4];
    uint8_t rx[4];

    uarte00_init();
    gpio_cs_init();
    spim20_init();

    printf("nRF54LM20 test start\n");

    clock_test();
    osc_test();
    grtc_test();

    tx[0] = 0x9Fu;
    tx[1] = 0xFFu;
    tx[2] = 0xFFu;
    tx[3] = 0xFFu;

    spim20_transfer(tx, rx, 4u);
    GPIO_OUTCLR(GPIO_P0_BASE) = 1u << 0;
    spim20_transfer(tx, rx, 4u);
    GPIO_OUTSET(GPIO_P0_BASE) = 1u << 0;

    printf("SPIM20 RX: %02x %02x %02x %02x\n", rx[0], rx[1], rx[2], rx[3]);

    rramc_test();

    printf("nRF54LM20 test done\n");

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
