/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Replays the register sequence the wolfBoot nRF5340 network core HAL uses:
 * CLOCK LFCLK start, RTC0 sleep, the IPC handshake with an absent
 * application core, NVMC word programming and 2 KB page erase, UARTE0.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;
extern void __libc_init_array(void);

#define REG32(a) (*(volatile uint32_t *)(a))

/* CLOCK */
#define CLOCK_BASE              0x41005000u
#define CLOCK_TASKS_LFCLKSTART  REG32(CLOCK_BASE + 0x008u)
#define CLOCK_EVENTS_LFCLKSTARTED REG32(CLOCK_BASE + 0x104u)
#define CLOCK_LFCLKSRC          REG32(CLOCK_BASE + 0x518u)

/* RTC0 */
#define RTC0_BASE               0x41011000u
#define RTC_TASKS_START         REG32(RTC0_BASE + 0x000u)
#define RTC_TASKS_STOP          REG32(RTC0_BASE + 0x004u)
#define RTC_TASKS_CLEAR         REG32(RTC0_BASE + 0x008u)
#define RTC_EVENTS_COMPARE0     REG32(RTC0_BASE + 0x140u)
#define RTC_EVTENSET            REG32(RTC0_BASE + 0x344u)
#define RTC_COUNTER             REG32(RTC0_BASE + 0x504u)
#define RTC_PRESCALER           REG32(RTC0_BASE + 0x508u)
#define RTC_CC0                 REG32(RTC0_BASE + 0x540u)
#define RTC_EVTENSET_COMPARE0   (1u << 16)

/* IPC */
#define IPC_BASE                0x41012000u
#define IPC_TASKS_SEND(n)       REG32(IPC_BASE + 0x000u + 4u * (n))
#define IPC_EVENTS_RECEIVE(n)   REG32(IPC_BASE + 0x100u + 4u * (n))
#define IPC_SEND_CNF(n)         REG32(IPC_BASE + 0x510u + 4u * (n))
#define IPC_RECEIVE_CNF(n)      REG32(IPC_BASE + 0x590u + 4u * (n))

/* NVMC */
#define NVMC_BASE               0x41080000u
#define NVMC_READY              REG32(NVMC_BASE + 0x400u)
#define NVMC_CONFIG             REG32(NVMC_BASE + 0x504u)
#define NVMC_CONFIG_WEN         1u
#define NVMC_CONFIG_EEN         2u
#define FLASH_BASE              0x01000000u
#define FLASH_SIZE              0x00040000u
#define FLASH_PAGE              0x800u
#define TEST_PAGE               (FLASH_BASE + FLASH_SIZE - 2u * FLASH_PAGE)

/* UARTE0 */
#define UARTE0_BASE             0x41013000u
#define UARTE_TASKS_STARTTX     REG32(UARTE0_BASE + 0x008u)
#define UARTE_EVENTS_ENDTX      REG32(UARTE0_BASE + 0x120u)
#define UARTE_ENABLE            REG32(UARTE0_BASE + 0x500u)
#define UARTE_PSEL_TXD          REG32(UARTE0_BASE + 0x50Cu)
#define UARTE_BAUDRATE          REG32(UARTE0_BASE + 0x524u)
#define UARTE_TXD_PTR           REG32(UARTE0_BASE + 0x544u)
#define UARTE_TXD_MAXCNT        REG32(UARTE0_BASE + 0x548u)
#define UARTE_CONFIG            REG32(UARTE0_BASE + 0x56Cu)

static volatile uint8_t uarte0_txbuf;
static int failures;

static void check(const char *name, int ok)
{
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        failures++;
    }
}

static void uarte0_init(void)
{
    UARTE_ENABLE = 0u;
    UARTE_PSEL_TXD = (1u << 5) | 1u; /* P1.01 */
    UARTE_BAUDRATE = 0x01D7E000u;
    UARTE_CONFIG = 0u;
    UARTE_ENABLE = 8u;
}

void uarte0_putc(char c)
{
    uarte0_txbuf = (uint8_t)c;
    UARTE_TXD_PTR = (uint32_t)&uarte0_txbuf;
    UARTE_TXD_MAXCNT = 1u;
    UARTE_EVENTS_ENDTX = 0u;
    UARTE_TASKS_STARTTX = 1u;
    while (UARTE_EVENTS_ENDTX == 0u) {
    }
}

static void clock_init(void)
{
    CLOCK_LFCLKSRC = 0u;
    CLOCK_TASKS_LFCLKSTART = 1u;
    while (CLOCK_EVENTS_LFCLKSTARTED == 0u) {
    }
    RTC_PRESCALER = 0u;
}

static void sleep_us(uint32_t usec)
{
    uint32_t comp = ((usec * 32768u) / 1000000u);
    if (comp == 0u) {
        comp = 1u;
    }
    RTC_TASKS_CLEAR = 1u;
    RTC_EVTENSET = RTC_EVTENSET_COMPARE0;
    RTC_EVENTS_COMPARE0 = 0u;
    RTC_CC0 = comp;
    RTC_TASKS_START = 1u;
    while (RTC_EVENTS_COMPARE0 == 0u) {
    }
    RTC_TASKS_STOP = 1u;
}

static void nvmc_wait(void)
{
    while (NVMC_READY == 0u) {
    }
}

static int page_is(uint32_t addr, uint8_t val)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint32_t i;
    for (i = 0; i < FLASH_PAGE; ++i) {
        if (p[i] != val) {
            return 0;
        }
    }
    return 1;
}

static void test_nvmc(void)
{
    volatile uint32_t *word = (volatile uint32_t *)(TEST_PAGE + 0x100u);
    volatile uint32_t *next = (volatile uint32_t *)(TEST_PAGE + FLASH_PAGE + 0x100u);

    /* erase both test pages first: the image loader leaves them blank, but
     * --persist runs may not */
    NVMC_CONFIG = NVMC_CONFIG_EEN;
    nvmc_wait();
    *(volatile uint32_t *)TEST_PAGE = 0xFFFFFFFFu;
    nvmc_wait();
    *(volatile uint32_t *)(TEST_PAGE + FLASH_PAGE) = 0xFFFFFFFFu;
    nvmc_wait();
    NVMC_CONFIG = 0u;
    check("pages blank", page_is(TEST_PAGE, 0xFFu) && page_is(TEST_PAGE + FLASH_PAGE, 0xFFu));

    NVMC_CONFIG = NVMC_CONFIG_WEN;
    nvmc_wait();
    *word = 0x12345678u;
    nvmc_wait();
    *next = 0xA5A5A5A5u;
    nvmc_wait();
    check("word program", *word == 0x12345678u && *next == 0xA5A5A5A5u);
    *word = 0x0F0F0F0Fu;
    nvmc_wait();
    check("program clears bits only", *word == 0x02040608u);
    NVMC_CONFIG = 0u;

    /* 2 KB page erase: the neighbour page keeps its data */
    NVMC_CONFIG = NVMC_CONFIG_EEN;
    nvmc_wait();
    *(volatile uint32_t *)TEST_PAGE = 0xFFFFFFFFu;
    nvmc_wait();
    NVMC_CONFIG = 0u;
    check("page erase", *word == 0xFFFFFFFFu && page_is(TEST_PAGE, 0xFFu));
    check("neighbour page kept", *next == 0xA5A5A5A5u);
}

static void test_ipc(void)
{
    /* Channel 1: NET send, channel 0: APP send (nothing answers) */
    IPC_SEND_CNF(1) = (1u << 1);
    IPC_RECEIVE_CNF(0) = (1u << 0);
    IPC_EVENTS_RECEIVE(0) = 0u;
    IPC_TASKS_SEND(1) = 1u;
    check("no application core event", IPC_EVENTS_RECEIVE(0) == 0u);
}

int main(void)
{
    uint32_t before;

    clock_init();
    uarte0_init();

    printf("nRF5340 network core test start\n");
    check("LFCLK started", CLOCK_EVENTS_LFCLKSTARTED == 1u);
    before = RTC_COUNTER;
    sleep_us(2000u);
    check("RTC0 compare", RTC_EVENTS_COMPARE0 == 1u && RTC_COUNTER != before);
    test_ipc();
    test_nvmc();

    if (failures == 0) {
        printf("ALL PASS\n");
        __asm volatile("bkpt #0x7f");
    } else {
        printf("FAILURES: %d\n", failures);
        __asm volatile("bkpt #0x7d");
    }
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
